// Unit tests for the DS SPU core. Run with `swift run spu-tests`.
//
// Most tests set a channel's timer to 0xFE00, which makes the channel advance
// exactly one hardware sample per output frame, and route it hard left at full
// volume. With those settings the mixer is lossless, so the left output equals
// the channel's current sample and tests can check the waveform directly.

#include "TestHarness.hpp"

#include <dsspu/Adpcm.hpp>
#include <dsspu/Spu.hpp>

#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

using namespace dsspu;

namespace {

constexpr uint32_t kBase = 0x02000000;
constexpr uint16_t kOneSamplePerFrame = 0x10000 - kTimerTicksPerOutputSample;

struct Rig {
    Spu spu;
    std::vector<uint8_t> memory;

    explicit Rig(std::vector<uint8_t> mem = {}) : memory(std::move(mem)) {
        spu.setMemory(memory, kBase);
        spu.setDegradeTo10Bit(false);
        spu.writeMasterControl({.volume = 127, .enable = true});
    }

    void playSample(int ch, Format format, RepeatMode repeat, uint16_t loopStartWords, uint32_t lengthWords,
                    ChannelControl control = {.pan = 0}) {
        spu.write32(channelRegister(ch, kChSad), kBase);
        spu.write16(channelRegister(ch, kChTmr), kOneSamplePerFrame);
        spu.write16(channelRegister(ch, kChPnt), loopStartWords);
        spu.write32(channelRegister(ch, kChLen), lengthWords);
        control.format = format;
        control.repeat = repeat;
        spu.writeChannelControl(ch, control);
    }

    void playPsg(int ch, uint8_t duty, uint16_t timer = kOneSamplePerFrame) {
        spu.write16(channelRegister(ch, kChTmr), timer);
        spu.writeChannelControl(ch, {.pan = 0, .duty = duty, .format = Format::PsgNoise});
    }

    // Renders and returns the left channel.
    std::vector<int16_t> renderLeft(size_t frames) {
        std::vector<int16_t> stereo(frames * 2);
        spu.render(stereo.data(), frames);
        std::vector<int16_t> left(frames);
        for (size_t i = 0; i < frames; ++i) left[i] = stereo[i * 2];
        return left;
    }

    std::vector<int16_t> renderStereo(size_t frames) {
        std::vector<int16_t> stereo(frames * 2);
        spu.render(stereo.data(), frames);
        return stereo;
    }
};

std::vector<uint8_t> toBytes(const std::vector<int16_t>& samples) {
    std::vector<uint8_t> bytes(samples.size() * 2);
    std::memcpy(bytes.data(), samples.data(), bytes.size());
    return bytes;
}

}  // namespace

// ---- PSG and noise ----------------------------------------------------------

TEST("PSG duty cycles 0-6 are (n+1)/8 high, duty 7 is always low") {
    for (uint8_t duty = 0; duty < 8; ++duty) {
        Rig rig;
        rig.playPsg(8, duty);
        const auto out = rig.renderLeft(16);
        for (size_t i = 0; i < out.size(); ++i) {
            const uint32_t step = i & 7;
            const bool high = duty != 7 && step >= 7u - duty;
            CHECK_EQ(out[i], int16_t(high ? 0x7FFF : -0x7FFF));
        }
    }
}

TEST("PSG and noise are silent on channels 0-7") {
    for (int ch = 0; ch < 8; ++ch) {
        Rig rig;
        rig.playPsg(ch, 3);
        for (int16_t s : rig.renderLeft(32)) CHECK_EQ(s, int16_t(0));
    }
}

TEST("PSG timer sets pitch: 440 Hz produces 440 cycles per second") {
    Rig rig;
    const uint16_t timer = timerForPsgFrequency(440.0);
    CHECK(std::abs(sampleRateForTimer(timer) / 8 - 440.0) < 0.1);

    rig.playPsg(8, 3, timer);
    const auto out = rig.renderLeft(size_t(kOutputSampleRate));
    int risingEdges = 0;
    for (size_t i = 1; i < out.size(); ++i) {
        if (out[i - 1] < 0 && out[i] > 0) ++risingEdges;
    }
    CHECK(std::abs(risingEdges - 440) <= 1);
}

TEST("Noise on channels 14-15 is a 15-bit LFSR with period 32767") {
    for (int ch : {14, 15}) {
        Rig rig;
        rig.spu.write16(channelRegister(ch, kChTmr), kOneSamplePerFrame);
        rig.spu.writeChannelControl(ch, {.pan = 0, .format = Format::PsgNoise});
        const auto out = rig.renderLeft(32767 * 2);

        // The LFSR starts at 0x7FFF, so bit 0 is set and the first output is low.
        CHECK_EQ(out[0], int16_t(-0x7FFF));

        bool periodic = true;
        for (size_t i = 0; i < 32767; ++i) periodic &= out[i] == out[i + 32767];
        CHECK(periodic);

        // 32767 = 7 * 31 * 151; the sequence must not repeat at any factor.
        for (size_t p : {7, 31, 151, 217, 1057, 4681}) {
            bool repeats = true;
            for (size_t i = 0; i < 32767 && repeats; ++i) repeats = out[i] == out[i + p];
            CHECK(!repeats);
        }
    }
}

// ---- PCM --------------------------------------------------------------------

TEST("PCM8 starts after 3 ticks, plays bytes as sample << 8 and loops") {
    std::vector<uint8_t> mem(16);
    for (size_t i = 0; i < mem.size(); ++i) mem[i] = uint8_t(int8_t(int(i) * 8 - 64));
    Rig rig(mem);
    rig.playSample(0, Format::Pcm8, RepeatMode::Loop, 0, 4);

    const auto out = rig.renderLeft(2 + 16 * 3);
    CHECK_EQ(out[0], int16_t(0));
    CHECK_EQ(out[1], int16_t(0));
    for (size_t i = 2; i < out.size(); ++i) {
        CHECK_EQ(out[i], int16_t(int8_t(mem[(i - 2) % 16]) * 256));
    }
}

TEST("PCM8 loop restarts at the loop start, not the beginning") {
    std::vector<uint8_t> mem(32);
    for (size_t i = 0; i < mem.size(); ++i) mem[i] = uint8_t(i);
    Rig rig(mem);
    rig.playSample(0, Format::Pcm8, RepeatMode::Loop, 2, 6);  // loop bytes 8-31

    const auto out = rig.renderLeft(2 + 32 + 24);
    for (size_t i = 0; i < 32; ++i) CHECK_EQ(out[2 + i], int16_t(i * 256));
    for (size_t i = 0; i < 24; ++i) CHECK_EQ(out[2 + 32 + i], int16_t((8 + i) * 256));
}

TEST("One-shot stops at the end and clears the busy bit") {
    std::vector<uint8_t> mem(16, 0x40);
    Rig rig(mem);
    rig.playSample(0, Format::Pcm8, RepeatMode::OneShot, 0, 4);

    const auto out = rig.renderLeft(2 + 16);
    CHECK_EQ(out.back(), int16_t(0x4000));
    CHECK(rig.spu.isChannelBusy(0));
    CHECK((rig.spu.read32(channelRegister(0, kChCnt)) >> 31) == 1);

    const auto after = rig.renderLeft(4);
    for (int16_t s : after) CHECK_EQ(s, int16_t(0));
    CHECK(!rig.spu.isChannelBusy(0));
    CHECK((rig.spu.read32(channelRegister(0, kChCnt)) >> 31) == 0);
}

TEST("PCM16 plays little-endian 16-bit samples") {
    std::vector<int16_t> samples = {0, 1000, -1000, 32767, -32768, 12345, -12345, 7};
    Rig rig(toBytes(samples));
    rig.playSample(0, Format::Pcm16, RepeatMode::Loop, 0, 4);

    const auto out = rig.renderLeft(2 + 16);
    for (size_t i = 0; i < 16; ++i) CHECK_EQ(out[2 + i], samples[i % 8]);
}

TEST("Samples shorter than 16 bytes play as silence") {
    std::vector<uint8_t> mem(12, 0x40);
    Rig rig(mem);
    rig.playSample(0, Format::Pcm8, RepeatMode::Loop, 0, 3);
    for (int16_t s : rig.renderLeft(32)) CHECK_EQ(s, int16_t(0));
}

// ---- ADPCM ------------------------------------------------------------------

TEST("ADPCM decodes a hand-computed nibble sequence") {
    // Header: value 0, index 0. Nibbles 7, 7, 0.
    std::vector<uint8_t> data = {0, 0, 0, 0, 0x77, 0x00};
    const auto out = adpcm::decode(data, 3);
    CHECK_EQ(out.size(), size_t(3));
    CHECK_EQ(out[0], int16_t(11));  // step 7: 0 + 1 + 3 + 7, index -> 8
    CHECK_EQ(out[1], int16_t(41));  // step 16: 2 + 4 + 8 + 16, index -> 16
    CHECK_EQ(out[2], int16_t(45));  // step 34: 34 >> 3, index -> 15
}

TEST("ADPCM clamps to +/-0x7FFF like the DS, not -0x8000") {
    adpcm::State state{-0x7FF0, adpcm::kMaxStepIndex};
    adpcm::decodeNibble(state, 0xF);
    CHECK_EQ(state.value, -0x7FFF);
    state = {0x7FF0, adpcm::kMaxStepIndex};
    adpcm::decodeNibble(state, 0x7);
    CHECK_EQ(state.value, 0x7FFF);
}

TEST("ADPCM encoder round-trips a sine wave with good quality") {
    std::vector<int16_t> sine(2000);
    for (size_t i = 0; i < sine.size(); ++i) {
        sine[i] = int16_t(16000 * std::sin(2 * std::numbers::pi * double(i) / 50));
    }
    const auto encoded = adpcm::encode(sine);
    CHECK_EQ(encoded.bytes.size() % 4, size_t(0));
    const auto decoded = adpcm::decode(encoded.bytes, sine.size());

    double signal = 0, noise = 0;
    for (size_t i = 0; i < sine.size(); ++i) {
        signal += double(sine[i]) * sine[i];
        noise += double(sine[i] - decoded[i]) * (sine[i] - decoded[i]);
    }
    const double snrDb = 10 * std::log10(signal / noise);
    CHECK(snrDb > 25);
}

TEST("ADPCM on the SPU matches the reference decoder, after the header delay") {
    std::vector<int16_t> pcm(200);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = int16_t((int(i) * 331) % 20000 - 10000);
    const auto encoded = adpcm::encode(pcm);
    const auto reference = adpcm::decode(encoded.bytes, pcm.size());

    Rig rig(encoded.bytes);
    rig.playSample(0, Format::Adpcm, RepeatMode::OneShot, 0, uint32_t(encoded.bytes.size() / 4));

    // 3 start ticks + 8 header nibbles; the first decoded sample lands on frame 10.
    const auto out = rig.renderLeft(10 + pcm.size());
    for (size_t i = 0; i < 10; ++i) CHECK_EQ(out[i], int16_t(0));
    for (size_t i = 0; i < pcm.size(); ++i) CHECK_EQ(out[10 + i], reference[i]);
}

TEST("ADPCM loops decode identically on every pass") {
    std::vector<int16_t> pcm(56);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = int16_t(int(i) * 500 - 14000);
    const auto encoded = adpcm::encode(pcm);
    const uint32_t totalWords = uint32_t(encoded.bytes.size() / 4);

    Rig rig(encoded.bytes);
    // Loop from the first data word (right after the header) to the end.
    rig.playSample(0, Format::Adpcm, RepeatMode::Loop, 1, totalWords - 1);

    const size_t period = (encoded.bytes.size() - adpcm::kHeaderBytes) * 2;
    const auto out = rig.renderLeft(10 + period * 3);
    bool identical = true;
    for (size_t i = 10; i < 10 + period * 2; ++i) identical &= out[i] == out[i + period];
    CHECK(identical);
}

// ---- Mixer ------------------------------------------------------------------

TEST("Volume, divider and pan follow the hardware fixed-point math") {
    std::vector<int16_t> samples(8, 0x4000);
    {
        Rig rig(toBytes(samples));
        rig.playSample(0, Format::Pcm16, RepeatMode::Loop, 0, 4,
                       {.volume = 64, .divider = VolumeDivider::Div2, .pan = 64});
        const auto out = rig.renderStereo(4);
        // ((0x4000 << 3) * 64 * 64 >> 10) * 128 >> 7 >> 8 = 0x800 on both sides.
        CHECK_EQ(out[6], int16_t(0x800));
        CHECK_EQ(out[7], int16_t(0x800));
    }
    {
        Rig rig(toBytes(samples));
        rig.playSample(0, Format::Pcm16, RepeatMode::Loop, 0, 4,
                       {.volume = 127, .divider = VolumeDivider::Div16, .pan = 0});
        const auto out = rig.renderStereo(4);
        CHECK_EQ(out[6], int16_t(0x4000 / 16));
        CHECK_EQ(out[7], int16_t(0));
    }
    {
        Rig rig(toBytes(samples));
        rig.playSample(0, Format::Pcm16, RepeatMode::Loop, 0, 4, {.volume = 127, .pan = 127});
        const auto out = rig.renderStereo(4);
        CHECK_EQ(out[6], int16_t(0));  // pan 127 is treated as 128: fully right
        CHECK_EQ(out[7], int16_t(0x4000));
    }
}

TEST("Mixer clips instead of wrapping") {
    Rig rig;
    rig.playPsg(8, 6);
    rig.playPsg(9, 6);
    const auto out = rig.renderLeft(8);
    CHECK_EQ(out[7], int16_t(0x7FFF));
    CHECK_EQ(out[0], int16_t(-0x8000));
}

TEST("Master enable and master volume") {
    Rig rig;
    rig.playPsg(8, 6);
    rig.spu.writeMasterControl({.volume = 64, .enable = true});
    auto out = rig.renderLeft(8);
    CHECK_EQ(out[7], int16_t((0x7FFF * 64) >> 7));

    rig.spu.writeMasterControl({.volume = 127, .enable = false});
    for (int16_t s : rig.renderLeft(8)) CHECK_EQ(s, int16_t(0));
}

TEST("10-bit output drops the low 6 bits") {
    Rig rig;
    rig.spu.setDegradeTo10Bit(true);
    rig.playPsg(8, 3);
    const auto out = rig.renderLeft(8);
    CHECK_EQ(out[7], int16_t(0x7FC0));
    CHECK_EQ(out[0], int16_t(-0x8000));
}

// ---- Registers --------------------------------------------------------------

TEST("Registers keep only writable bits") {
    Spu spu;
    spu.write32(channelRegister(3, kChCnt), 0xFFFFFFFF & ~(1u << 31));
    CHECK_EQ(spu.read32(channelRegister(3, kChCnt)), uint32_t(0x7F7F837F));
    spu.write32(channelRegister(3, kChSad), 0xFFFFFFFF);
    CHECK_EQ(spu.read32(channelRegister(3, kChSad)), uint32_t(0x07FFFFFC));
    spu.write32(channelRegister(3, kChLen), 0xFFFFFFFF);
    CHECK_EQ(spu.read32(channelRegister(3, kChLen)), uint32_t(0x001FFFFF));
}

TEST("Clearing the start bit stops a channel immediately") {
    Rig rig;
    rig.playPsg(8, 6);
    rig.renderLeft(8);
    rig.spu.writeChannelControl(8, {.pan = 0, .duty = 6, .format = Format::PsgNoise, .start = false});
    for (int16_t s : rig.renderLeft(8)) CHECK_EQ(s, int16_t(0));
}

TEST("Rewriting the control register while playing does not restart the channel") {
    Rig rig;
    rig.playPsg(8, 0);  // high only on step 7
    rig.renderLeft(3);
    rig.spu.writeChannelControl(8, {.volume = 127, .pan = 0, .duty = 0, .format = Format::PsgNoise});
    const auto out = rig.renderLeft(5);
    // Continues at step 3 rather than restarting at step 0, so step 7 is frame 4.
    CHECK_EQ(out[4], int16_t(0x7FFF));
}

TEST("Byte writes to the control register start a channel") {
    Rig rig;
    rig.spu.write16(channelRegister(8, kChTmr), kOneSamplePerFrame);
    const uint32_t control = ChannelControl{.pan = 0, .duty = 6, .format = Format::PsgNoise}.encode();
    for (uint32_t i = 0; i < 4; ++i) rig.spu.write8(channelRegister(8, kChCnt) + i, uint8_t(control >> (8 * i)));
    const auto out = rig.renderLeft(8);
    CHECK_EQ(out[7], int16_t(0x7FFF));
}

int main() { return test::runAll(); }
