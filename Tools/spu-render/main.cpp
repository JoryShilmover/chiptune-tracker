// spu-render: renders demo songs through the DS SPU core to a WAV file.
//
// The demo is driven the same way the tracker's sequencer will drive the SPU:
// a tick function runs 60 times a second and only writes SPU registers.
// Envelopes, arpeggios and drums are all done in software on top of that,
// exactly as a DS sound driver would.

#include "Wav.hpp"

#include <dsspu/Adpcm.hpp>
#include <dsspu/Spu.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <numbers>
#include <string>
#include <vector>

using namespace dsspu;

namespace {

constexpr uint32_t kMemBase = 0x02000000;
constexpr double kTickRate = 60.0;
constexpr int kTicksPerStep = 7;  // ~128 BPM in 16th notes

double noteHz(int midiNote) { return 440.0 * std::pow(2.0, (midiNote - 69) / 12.0); }

// Sample memory for the demo: a looping single-cycle saw (PCM8) and a kick
// drum (ADPCM).
struct SampleBank {
    std::vector<uint8_t> memory;
    uint32_t sawAddress = 0;
    uint32_t sawWords = 0;
    static constexpr uint32_t kSawLength = 32;
    uint32_t kickAddress = 0;
    uint32_t kickWords = 0;
    static constexpr double kKickRate = 16384;

    SampleBank() {
        for (uint32_t i = 0; i < kSawLength; ++i) {
            memory.push_back(uint8_t(int8_t(int(i) * 256 / int(kSawLength) - 128)));
        }
        sawAddress = kMemBase;
        sawWords = kSawLength / 4;

        std::vector<int16_t> kick(size_t(kKickRate * 0.3));
        double phase = 0;
        for (size_t i = 0; i < kick.size(); ++i) {
            const double t = double(i) / kKickRate;
            const double freq = 40 + 110 * std::exp(-t * 30);
            phase += 2 * std::numbers::pi * freq / kKickRate;
            kick[i] = int16_t(30000 * std::exp(-t * 10) * std::sin(phase));
        }
        const auto encoded = adpcm::encode(kick);
        while (memory.size() % 4) memory.push_back(0);
        kickAddress = kMemBase + uint32_t(memory.size());
        kickWords = uint32_t(encoded.bytes.size() / 4);
        memory.insert(memory.end(), encoded.bytes.begin(), encoded.bytes.end());
    }
};

class Demo {
public:
    explicit Demo(Spu& spu, const SampleBank& bank) : spu_(spu), bank_(bank) {}

    void keyOn(int ch, ChannelControl control, uint16_t timer) {
        spu_.write16(channelRegister(ch, kChTmr), timer);
        control.start = false;
        spu_.writeChannelControl(ch, control);
        control.start = true;
        spu_.writeChannelControl(ch, control);
    }

    void setSample(int ch, uint32_t address, uint16_t loopStartWords, uint32_t lengthWords) {
        spu_.write32(channelRegister(ch, kChSad), address);
        spu_.write16(channelRegister(ch, kChPnt), loopStartWords);
        spu_.write32(channelRegister(ch, kChLen), lengthWords);
    }

    // Updates volume without retriggering (the start bit stays set).
    void setVolume(int ch, ChannelControl control, double volume) {
        control.volume = uint8_t(std::clamp(volume, 0.0, 127.0));
        control.start = spu_.isChannelBusy(ch);
        spu_.writeChannelControl(ch, control);
    }

    void setTimer(int ch, uint16_t timer) { spu_.write16(channelRegister(ch, kChTmr), timer); }

    // --- Parts ---

    void lead(int tick) {
        static const int kMelody[32] = {69, -1, 72, 76, 74, -1, 72, -1, 71, -1, 72, 74, 76, -1, 79, -1,
                                        81, -1, 79, 76, 74, -1, 72, -1, 71, -1, 67, 69, 71, -1, -1, -1};
        const ChannelControl ctl{.pan = 48, .duty = 2, .repeat = RepeatMode::Loop, .format = Format::PsgNoise};
        const int step = tick / kTicksPerStep;
        const int t = tick % kTicksPerStep;
        const int note = kMelody[step % 32];
        if (t == 0 && note >= 0) {
            leadNote_ = note;
            leadAge_ = 0;
            keyOn(8, ctl, timerForPsgFrequency(noteHz(note)));
        }
        // Delayed vibrato after 8 ticks.
        if (leadAge_ > 8) {
            const double cents = 15 * std::sin(leadAge_ * 0.5);
            setTimer(8, timerForPsgFrequency(noteHz(leadNote_) * std::pow(2.0, cents / 1200)));
        }
        setVolume(8, ctl, 55 * std::pow(0.97, leadAge_));
        ++leadAge_;
    }

    void arpeggio(int tick) {
        static const int kChords[4][3] = {{57, 60, 64}, {53, 57, 60}, {55, 59, 62}, {52, 55, 59}};
        const ChannelControl ctl{.volume = 22, .pan = 80, .duty = 0, .format = Format::PsgNoise};
        const int bar = (tick / (kTicksPerStep * 8)) % 4;
        const int note = kChords[bar][tick % 3] + 12;
        if (tick == 0) keyOn(9, ctl, timerForPsgFrequency(noteHz(note)));
        else setTimer(9, timerForPsgFrequency(noteHz(note)));
    }

    void bass(int tick) {
        static const int kRoots[4] = {45, 41, 43, 40};
        const ChannelControl ctl{.pan = 64, .repeat = RepeatMode::Loop, .format = Format::Pcm8};
        const int step = tick / kTicksPerStep;
        const int t = tick % kTicksPerStep;
        if (t == 0 && step % 2 == 0) {
            const int root = kRoots[(step / 8) % 4] + ((step % 4 == 2) ? 12 : 0);
            setSample(0, bank_.sawAddress, 0, bank_.sawWords);
            keyOn(0, ctl, timerForSampleRate(noteHz(root) * SampleBank::kSawLength));
            bassAge_ = 0;
        }
        setVolume(0, ctl, 70 * std::pow(0.92, bassAge_++));
    }

    void drums(int tick, bool kick, bool noise) {
        const int step = tick / kTicksPerStep;
        const int t = tick % kTicksPerStep;
        if (kick && t == 0 && step % 4 == 0) {
            setSample(1, bank_.kickAddress, 0, bank_.kickWords);
            keyOn(1, {.volume = 90, .pan = 64, .repeat = RepeatMode::OneShot, .format = Format::Adpcm},
                  timerForSampleRate(SampleBank::kKickRate));
        }

        if (!noise) return;
        const ChannelControl hat{.pan = 90, .format = Format::PsgNoise};
        const ChannelControl snare{.pan = 60, .format = Format::PsgNoise};
        if (t == 0) {
            if (step % 8 == 4) {
                keyOn(15, snare, timerForSampleRate(12000));
                snareAge_ = 0;
            } else if (step % 2 == 1) {
                keyOn(14, hat, timerForSampleRate(40000));
                hatAge_ = 0;
            }
        }
        setVolume(14, hat, 30 * std::pow(0.55, hatAge_++));
        setVolume(15, snare, 55 * std::pow(0.75, snareAge_++));
    }

private:
    Spu& spu_;
    const SampleBank& bank_;
    int leadNote_ = 69;
    int leadAge_ = 1000;
    int bassAge_ = 1000;
    int hatAge_ = 1000;
    int snareAge_ = 1000;
};

std::vector<int16_t> renderSong(Spu& spu, double seconds, const std::function<void(int)>& onTick) {
    const auto totalFrames = size_t(seconds * kOutputSampleRate);
    const double framesPerTick = kOutputSampleRate / kTickRate;
    std::vector<int16_t> out(totalFrames * 2);

    size_t frame = 0;
    double nextTick = 0;
    int tick = 0;
    while (frame < totalFrames) {
        if (double(frame) >= nextTick) {
            onTick(tick++);
            nextTick += framesPerTick;
        }
        const size_t end = std::min(totalFrames, size_t(std::ceil(nextTick)));
        spu.render(out.data() + frame * 2, end - frame);
        frame = end;
    }
    return out;
}

void usage() {
    std::puts(
        "usage: spu-render [demo] [-o output.wav] [--seconds N] [--16bit]\n"
        "\n"
        "demos: all (default), psg, noise, pcm, adpcm\n"
        "  --16bit  skip the DS's 10-bit output stage for a clean reference render");
}

}  // namespace

int main(int argc, char** argv) {
    std::string demo = "all";
    std::string output;
    double seconds = 64 * kTicksPerStep / kTickRate;  // the 32-step loop, twice
    bool degrade = true;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else if (arg == "-o" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (arg == "--16bit") {
            degrade = false;
        } else if (arg[0] != '-') {
            demo = arg;
        } else {
            usage();
            return 2;
        }
    }
    if (output.empty()) output = "demo-" + demo + ".wav";

    const bool all = demo == "all";
    const bool psg = all || demo == "psg";
    const bool noise = all || demo == "noise";
    const bool pcm = all || demo == "pcm";
    const bool adpcmDemo = all || demo == "adpcm";
    if (!psg && !noise && !pcm && !adpcmDemo) {
        std::fprintf(stderr, "unknown demo: %s\n", demo.c_str());
        usage();
        return 2;
    }

    SampleBank bank;
    Spu spu;
    spu.setMemory(bank.memory, kMemBase);
    spu.setDegradeTo10Bit(degrade);
    spu.write16(kRegSoundBias, 0x200);
    spu.writeMasterControl({.volume = 100, .enable = true});

    Demo song(spu, bank);
    const auto audio = renderSong(spu, seconds, [&](int tick) {
        if (psg) {
            song.lead(tick);
            song.arpeggio(tick);
        }
        if (pcm) song.bass(tick);
        if (noise || adpcmDemo) song.drums(tick, adpcmDemo, noise);
    });

    // WAV needs an integer rate; 32728 Hz vs the true ~32728.5 Hz is a
    // 0.0015% pitch difference.
    if (!writeWav(output, audio, uint32_t(kOutputSampleRate))) {
        std::fprintf(stderr, "failed to write %s\n", output.c_str());
        return 1;
    }
    std::printf("wrote %s (%.1f s, %u Hz, %s output)\n", output.c_str(), seconds, uint32_t(kOutputSampleRate),
                degrade ? "10-bit" : "16-bit");
    return 0;
}
