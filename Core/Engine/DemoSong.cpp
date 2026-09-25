#include "tracker/DemoSong.hpp"

#include <dsspu/Adpcm.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace tracker {

using namespace dsspu;

namespace {

constexpr uint32_t kSawLength = 32;
constexpr double kKickRate = 16384;

double noteHz(int midiNote) { return 440.0 * std::pow(2.0, (midiNote - 69) / 12.0); }

void keyOn(Spu& spu, int ch, ChannelControl control, uint16_t timer) {
    spu.write16(channelRegister(ch, kChTmr), timer);
    control.start = false;
    spu.writeChannelControl(ch, control);
    control.start = true;
    spu.writeChannelControl(ch, control);
}

void setSample(Spu& spu, int ch, uint32_t address, uint16_t loopStartWords, uint32_t lengthWords) {
    spu.write32(channelRegister(ch, kChSad), address);
    spu.write16(channelRegister(ch, kChPnt), loopStartWords);
    spu.write32(channelRegister(ch, kChLen), lengthWords);
}

// Updates volume without retriggering: the start bit keeps its current value.
void setVolume(Spu& spu, int ch, ChannelControl control, double volume) {
    control.volume = uint8_t(std::clamp(volume, 0.0, 127.0));
    control.start = spu.isChannelBusy(ch);
    spu.writeChannelControl(ch, control);
}

void setTimer(Spu& spu, int ch, uint16_t timer) { spu.write16(channelRegister(ch, kChTmr), timer); }

}  // namespace

DemoSong::DemoSong() : DemoSong(Parts{}) {}

DemoSong::DemoSong(Parts parts) : parts_(parts) {
    // A single-cycle saw wave (PCM8), looped for the bass.
    for (uint32_t i = 0; i < kSawLength; ++i) {
        memory_.push_back(uint8_t(int8_t(int(i) * 256 / int(kSawLength) - 128)));
    }
    sawAddress_ = kSampleMemoryBase;
    sawWords_ = kSawLength / 4;

    // A pitch-swept sine kick, ADPCM encoded.
    std::vector<int16_t> kick(size_t(kKickRate * 0.3));
    double phase = 0;
    for (size_t i = 0; i < kick.size(); ++i) {
        const double t = double(i) / kKickRate;
        const double freq = 40 + 110 * std::exp(-t * 30);
        phase += 2 * std::numbers::pi * freq / kKickRate;
        kick[i] = int16_t(30000 * std::exp(-t * 10) * std::sin(phase));
    }
    const auto encoded = adpcm::encode(kick);
    while (memory_.size() % 4) memory_.push_back(0);
    kickAddress_ = kSampleMemoryBase + uint32_t(memory_.size());
    kickWords_ = uint32_t(encoded.bytes.size() / 4);
    memory_.insert(memory_.end(), encoded.bytes.begin(), encoded.bytes.end());
}

void DemoSong::reset(Spu& spu) {
    spu.write16(kRegSoundBias, 0x200);
    spu.writeMasterControl({.volume = 100, .enable = true});
    leadNote_ = 69;
    leadAge_ = bassAge_ = hatAge_ = snareAge_ = 1000;
}

void DemoSong::tick(Spu& spu, uint32_t tick) {
    if (parts_.psg) {
        lead(spu, tick);
        arpeggio(spu, tick);
    }
    if (parts_.pcm) bass(spu, tick);
    if (parts_.noise || parts_.adpcm) drums(spu, tick);
}

void DemoSong::lead(Spu& spu, uint32_t tick) {
    static constexpr int kMelody[kLoopSteps] = {69, -1, 72, 76, 74, -1, 72, -1, 71, -1, 72, 74, 76, -1, 79, -1,
                                                81, -1, 79, 76, 74, -1, 72, -1, 71, -1, 67, 69, 71, -1, -1, -1};
    const ChannelControl ctl{.pan = 48, .duty = 2, .repeat = RepeatMode::Loop, .format = Format::PsgNoise};
    const uint32_t step = tick / kTicksPerStep;
    const int note = kMelody[step % kLoopSteps];
    if (tick % kTicksPerStep == 0 && note >= 0) {
        leadNote_ = note;
        leadAge_ = 0;
        keyOn(spu, 8, ctl, timerForPsgFrequency(noteHz(note)));
    }
    // Delayed vibrato after 8 ticks.
    if (leadAge_ > 8) {
        const double cents = 15 * std::sin(leadAge_ * 0.5);
        setTimer(spu, 8, timerForPsgFrequency(noteHz(leadNote_) * std::pow(2.0, cents / 1200)));
    }
    setVolume(spu, 8, ctl, 55 * std::pow(0.97, leadAge_));
    ++leadAge_;
}

void DemoSong::arpeggio(Spu& spu, uint32_t tick) {
    static constexpr int kChords[4][3] = {{57, 60, 64}, {53, 57, 60}, {55, 59, 62}, {52, 55, 59}};
    const ChannelControl ctl{.volume = 22, .pan = 80, .duty = 0, .format = Format::PsgNoise};
    const uint32_t bar = (tick / (kTicksPerStep * 8)) % 4;
    const int note = kChords[bar][tick % 3] + 12;
    if (tick == 0) keyOn(spu, 9, ctl, timerForPsgFrequency(noteHz(note)));
    else setTimer(spu, 9, timerForPsgFrequency(noteHz(note)));
}

void DemoSong::bass(Spu& spu, uint32_t tick) {
    static constexpr int kRoots[4] = {45, 41, 43, 40};
    const ChannelControl ctl{.pan = 64, .repeat = RepeatMode::Loop, .format = Format::Pcm8};
    const uint32_t step = tick / kTicksPerStep;
    if (tick % kTicksPerStep == 0 && step % 2 == 0) {
        const int root = kRoots[(step / 8) % 4] + ((step % 4 == 2) ? 12 : 0);
        setSample(spu, 0, sawAddress_, 0, sawWords_);
        keyOn(spu, 0, ctl, timerForSampleRate(noteHz(root) * kSawLength));
        bassAge_ = 0;
    }
    setVolume(spu, 0, ctl, 70 * std::pow(0.92, bassAge_++));
}

void DemoSong::drums(Spu& spu, uint32_t tick) {
    const uint32_t step = tick / kTicksPerStep;
    const bool onStep = tick % kTicksPerStep == 0;
    if (parts_.adpcm && onStep && step % 4 == 0) {
        setSample(spu, 1, kickAddress_, 0, kickWords_);
        keyOn(spu, 1, {.volume = 90, .pan = 64, .repeat = RepeatMode::OneShot, .format = Format::Adpcm},
              timerForSampleRate(kKickRate));
    }

    if (!parts_.noise) return;
    const ChannelControl hat{.pan = 90, .format = Format::PsgNoise};
    const ChannelControl snare{.pan = 60, .format = Format::PsgNoise};
    if (onStep) {
        if (step % 8 == 4) {
            keyOn(spu, 15, snare, timerForSampleRate(12000));
            snareAge_ = 0;
        } else if (step % 2 == 1) {
            keyOn(spu, 14, hat, timerForSampleRate(40000));
            hatAge_ = 0;
        }
    }
    setVolume(spu, 14, hat, 30 * std::pow(0.55, hatAge_++));
    setVolume(spu, 15, snare, 55 * std::pow(0.75, snareAge_++));
}

}  // namespace tracker
