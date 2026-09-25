#include "dsspu/Spu.hpp"

#include "dsspu/Adpcm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dsspu {

namespace {

// Writable bits of each channel register.
constexpr uint32_t kCntMask = 0xFF7F837F;
constexpr uint32_t kSadMask = 0x07FFFFFC;
constexpr uint32_t kLenMask = 0x001FFFFF;
constexpr uint32_t kStartBit = 1u << 31;

// Sample formats shorter than this (loop start + length) play as silence.
constexpr uint32_t kMinSampleBytes = 16;

constexpr uint32_t kSoundCntOffset = kRegSoundCnt - kRegChannelBase;
constexpr uint32_t kSoundBiasOffset = kRegSoundBias - kRegChannelBase;

// Volume and pan registers are 7-bit, but the hardware treats 127 as 128 so
// that full volume and hard pan are lossless.
constexpr uint32_t expand7Bit(uint32_t value) { return value == 127 ? 128 : value; }

}  // namespace

uint16_t timerForSampleRate(double sampleRateHz) {
    if (sampleRateHz <= 0) return 0;
    const double divisor = std::round(double(kTimerClockHz) / sampleRateHz);
    return uint16_t(0x10000 - uint32_t(std::clamp(divisor, 1.0, 65536.0)));
}

uint16_t timerForPsgFrequency(double frequencyHz) { return timerForSampleRate(frequencyHz * 8); }

double sampleRateForTimer(uint16_t timer) { return double(kTimerClockHz) / double(0x10000 - uint32_t(timer)); }

// ---- Setup and register access ----------------------------------------------

Spu::Spu() { reset(); }

void Spu::reset() {
    registers_.fill(0);
    channels_.fill(Channel{});
    channelPeaks_.fill(0);
    for (int i = 0; i < kChannelCount; ++i) decodeChannel(i);
    masterVolume_ = 0;
    masterEnable_ = false;
    bias_ = 0;
}

void Spu::setMemory(std::span<const uint8_t> memory, uint32_t baseAddress) {
    memory_ = memory;
    memoryBase_ = baseAddress;
}

void Spu::write8(uint32_t address, uint8_t value) {
    const uint32_t offset = address - kRegChannelBase;
    if (offset >= registers_.size()) return;
    const uint32_t oldControl = offset < kSoundCntOffset ? channels_[offset >> 4].control : 0;
    registers_[offset] = value;
    onRegisterWrite(offset, 1, oldControl);
}

void Spu::write16(uint32_t address, uint16_t value) {
    const uint32_t offset = (address & ~1u) - kRegChannelBase;
    if (offset >= registers_.size()) return;
    const uint32_t oldControl = offset < kSoundCntOffset ? channels_[offset >> 4].control : 0;
    registers_[offset] = uint8_t(value);
    registers_[offset + 1] = uint8_t(value >> 8);
    onRegisterWrite(offset, 2, oldControl);
}

void Spu::write32(uint32_t address, uint32_t value) {
    const uint32_t offset = (address & ~3u) - kRegChannelBase;
    if (offset >= registers_.size()) return;
    const uint32_t oldControl = offset < kSoundCntOffset ? channels_[offset >> 4].control : 0;
    for (uint32_t i = 0; i < 4; ++i) registers_[offset + i] = uint8_t(value >> (8 * i));
    onRegisterWrite(offset, 4, oldControl);
}

uint8_t Spu::read8(uint32_t address) const {
    const uint32_t offset = address - kRegChannelBase;
    return offset < registers_.size() ? registers_[offset] : 0;
}

uint16_t Spu::read16(uint32_t address) const {
    return uint16_t(read8(address & ~1u) | read8((address & ~1u) + 1) << 8);
}

uint32_t Spu::read32(uint32_t address) const {
    return uint32_t(read16(address & ~3u)) | uint32_t(read16((address & ~3u) + 2)) << 16;
}

bool Spu::isChannelBusy(int channel) const {
    return channel >= 0 && channel < kChannelCount && (channels_[size_t(channel)].control & kStartBit);
}

void Spu::onRegisterWrite(uint32_t offset, uint32_t size, uint32_t oldChannelControl) {
    if (offset < kSoundCntOffset) {
        const int index = int(offset >> 4);
        decodeChannel(index);
        Channel& ch = channels_[size_t(index)];
        const bool wroteControl = (offset & 0xF) < kChCnt + 4;
        if (wroteControl) {
            const bool wasOn = oldChannelControl & kStartBit;
            const bool isOn = ch.control & kStartBit;
            // Starting takes effect on the next mixer tick. Clearing the bit
            // stops the channel immediately.
            if (isOn && !wasOn) ch.keyOn = true;
            if (!isOn) ch.keyOn = false;
        }
        return;
    }

    (void)size;
    if (offset == kSoundCntOffset || offset == kSoundCntOffset + 1) {
        const uint16_t cnt = uint16_t(registers_[kSoundCntOffset] | registers_[kSoundCntOffset + 1] << 8);
        masterVolume_ = expand7Bit(cnt & 0x7F);
        masterEnable_ = cnt & 0x8000;
    } else if (offset == kSoundBiasOffset || offset == kSoundBiasOffset + 1) {
        bias_ = uint32_t(registers_[kSoundBiasOffset] | registers_[kSoundBiasOffset + 1] << 8) & 0x3FF;
    }
}

void Spu::decodeChannel(int index) {
    Channel& ch = channels_[size_t(index)];
    uint8_t* regs = &registers_[size_t(index) * kRegChannelStride];
    auto load32 = [regs](uint32_t off) {
        return uint32_t(regs[off]) | uint32_t(regs[off + 1]) << 8 | uint32_t(regs[off + 2]) << 16 |
               uint32_t(regs[off + 3]) << 24;
    };
    auto store32 = [regs](uint32_t off, uint32_t value) {
        for (uint32_t i = 0; i < 4; ++i) regs[off + i] = uint8_t(value >> (8 * i));
    };

    // Keep only the writable bits so reads return what the hardware would.
    ch.control = load32(kChCnt) & kCntMask;
    ch.sourceAddress = load32(kChSad) & kSadMask;
    const uint32_t length = load32(kChLen) & kLenMask;
    store32(kChCnt, ch.control);
    store32(kChSad, ch.sourceAddress);
    store32(kChLen, length);

    ch.timerReload = uint16_t(regs[kChTmr] | regs[kChTmr + 1] << 8);
    ch.loopStartBytes = uint32_t(regs[kChPnt] | regs[kChPnt + 1] << 8) * 4;
    ch.lengthBytes = length * 4;

    ch.volume = expand7Bit(ch.control & 0x7F);
    static constexpr uint32_t kVolumeShift[4] = {4, 3, 2, 0};  // divide by 1, 2, 4, 16
    ch.volumeShift = kVolumeShift[(ch.control >> 8) & 0x3];
    ch.pan = expand7Bit((ch.control >> 16) & 0x7F);
}

// ---- Channel playback -------------------------------------------------------

void Spu::start(Channel& ch) {
    ch.timer = ch.timerReload;
    // Sample formats take three timer ticks before the first sample plays;
    // PSG and noise take one.
    ch.position = ch.format() == Format::PsgNoise ? -1 : -3;
    ch.noiseLfsr = 0x7FFF;
    ch.sample = 0;
    ch.adpcmValue = ch.adpcmIndex = 0;
    ch.adpcmLoopValue = ch.adpcmLoopIndex = 0;
    ch.adpcmByte = 0;
}

void Spu::stop(Channel& ch, int index) {
    ch.sample = 0;
    ch.control &= ~kStartBit;
    registers_[size_t(index) * kRegChannelStride + kChCnt + 3] &= 0x7F;
}

int32_t Spu::runChannel(Channel& ch, int index) {
    if (!(ch.control & kStartBit)) return 0;

    const Format format = ch.format();
    if (format == Format::PsgNoise) {
        if (index < kFirstPsgChannel) return 0;
    } else if (ch.loopStartBytes + ch.lengthBytes < kMinSampleBytes) {
        return 0;
    }

    if (ch.keyOn) {
        start(ch);
        ch.keyOn = false;
    }

    ch.timer += kTimerTicksPerOutputSample;
    while (ch.timer >> 16) {
        ch.timer = ch.timerReload + (ch.timer - 0x10000);
        nextSample(ch, index);
        if (!(ch.control & kStartBit)) break;
    }

    // 16.4 fixed point after the divider, then scaled by volume (N/128).
    return (int32_t(ch.sample) << ch.volumeShift) * int32_t(ch.volume);
}

void Spu::nextSample(Channel& ch, int index) {
    switch (ch.format()) {
        case Format::Pcm8: nextPcm8(ch, index); break;
        case Format::Pcm16: nextPcm16(ch, index); break;
        case Format::Adpcm: nextAdpcm(ch, index); break;
        case Format::PsgNoise:
            if (index >= kFirstNoiseChannel) nextNoise(ch);
            else nextPsg(ch);
            break;
    }
}

// Position counts bytes.
void Spu::nextPcm8(Channel& ch, int index) {
    if (++ch.position < 0) return;
    if (uint32_t(ch.position) >= ch.loopStartBytes + ch.lengthBytes) {
        if (ch.repeat() & 1) {
            ch.position = int32_t(ch.loopStartBytes);
        } else if (ch.repeat() & 2) {
            stop(ch, index);
            return;
        }
    }
    ch.sample = int16_t(int8_t(memRead8(ch.sourceAddress + uint32_t(ch.position))) * 256);
}

// Position counts 16-bit samples.
void Spu::nextPcm16(Channel& ch, int index) {
    if (++ch.position < 0) return;
    if (uint32_t(ch.position) * 2 >= ch.loopStartBytes + ch.lengthBytes) {
        if (ch.repeat() & 1) {
            ch.position = int32_t(ch.loopStartBytes / 2);
        } else if (ch.repeat() & 2) {
            stop(ch, index);
            return;
        }
    }
    ch.sample = memRead16(ch.sourceAddress + uint32_t(ch.position) * 2);
}

// Position counts nibbles. Nibbles 0-7 are the 4-byte header, which takes
// eight timer ticks to read but produces no output.
void Spu::nextAdpcm(Channel& ch, int index) {
    if (++ch.position < 8) {
        if (ch.position == 0) {
            const uint32_t header = memRead32(ch.sourceAddress);
            ch.adpcmValue = int16_t(header & 0xFFFF);
            ch.adpcmIndex = std::min<int32_t>((header >> 16) & 0x7F, adpcm::kMaxStepIndex);
            ch.adpcmLoopValue = ch.adpcmValue;
            ch.adpcmLoopIndex = ch.adpcmIndex;
        }
        return;
    }

    if (uint32_t(ch.position >> 1) >= ch.loopStartBytes + ch.lengthBytes) {
        if (ch.repeat() & 1) {
            // Jump back and restore the decoder state saved when the loop start
            // was first decoded, so every pass decodes identically.
            ch.position = int32_t(ch.loopStartBytes << 1);
            ch.adpcmValue = ch.adpcmLoopValue;
            ch.adpcmIndex = ch.adpcmLoopIndex;
            ch.adpcmByte = memRead8(ch.sourceAddress + ch.loopStartBytes);
            ch.sample = int16_t(ch.adpcmValue);
            return;
        }
        if (ch.repeat() & 2) {
            stop(ch, index);
            return;
        }
    }

    if (!(ch.position & 1)) {
        ch.adpcmByte = memRead8(ch.sourceAddress + uint32_t(ch.position >> 1));
    } else {
        ch.adpcmByte >>= 4;
    }

    adpcm::State state{ch.adpcmValue, ch.adpcmIndex};
    adpcm::decodeNibble(state, ch.adpcmByte & 0xF);
    ch.adpcmValue = state.value;
    ch.adpcmIndex = state.index;

    if (ch.position == int32_t(ch.loopStartBytes << 1)) {
        ch.adpcmLoopValue = ch.adpcmValue;
        ch.adpcmLoopIndex = ch.adpcmIndex;
    }
    ch.sample = int16_t(ch.adpcmValue);
}

// 8-step square wave. Duty n (0-6) is high for the last n+1 steps; duty 7 is
// always low.
void Spu::nextPsg(Channel& ch) {
    ++ch.position;
    const uint32_t duty = ch.duty();
    const uint32_t step = uint32_t(ch.position) & 7;
    const bool high = duty != 7 && step >= 7 - duty;
    ch.sample = high ? 0x7FFF : -0x7FFF;
}

// 15-bit linear-feedback shift register.
void Spu::nextNoise(Channel& ch) {
    if (ch.noiseLfsr & 1) {
        ch.noiseLfsr = uint16_t((ch.noiseLfsr >> 1) ^ 0x6000);
        ch.sample = -0x7FFF;
    } else {
        ch.noiseLfsr >>= 1;
        ch.sample = 0x7FFF;
    }
}

// ---- Mixer ------------------------------------------------------------------

void Spu::render(int16_t* interleavedStereo, size_t frames) {
    for (size_t frame = 0; frame < frames; ++frame) {
        int64_t left = 0;
        int64_t right = 0;
        for (int i = 0; i < kChannelCount; ++i) {
            Channel& ch = channels_[size_t(i)];
            const int64_t value = runChannel(ch, i);
            // Full scale is 0x7FFF << 4 (divider) * 128 (volume) = 0x7FFF << 11.
            const auto level = uint16_t(std::min<int64_t>(std::abs(value) >> 11, 0x7FFF));
            channelPeaks_[size_t(i)] = std::max(channelPeaks_[size_t(i)], level);
            // Pan (N/128), then drop 10 fraction bits.
            left += (value * int64_t(128 - ch.pan)) >> 10;
            right += (value * int64_t(ch.pan)) >> 10;
        }

        int64_t outL = 0;
        int64_t outR = 0;
        if (masterEnable_) {
            // Master volume (N/128), then down to 16 bits.
            outL = ((left * int64_t(masterVolume_)) >> 7) >> 8;
            outR = ((right * int64_t(masterVolume_)) >> 7) >> 8;
        }

        // The hardware clips around SOUNDBIAS. Every game uses the standard
        // bias of 0x200 (mid-scale), which is what this assumes.
        outL = std::clamp<int64_t>(outL, -0x8000, 0x7FFF);
        outR = std::clamp<int64_t>(outR, -0x8000, 0x7FFF);
        if (degradeTo10Bit_) {
            outL &= ~int64_t(0x3F);
            outR &= ~int64_t(0x3F);
        }

        interleavedStereo[frame * 2] = int16_t(outL);
        interleavedStereo[frame * 2 + 1] = int16_t(outR);
    }
}

// ---- Memory -----------------------------------------------------------------

uint8_t Spu::memRead8(uint32_t address) const {
    const uint32_t offset = address - memoryBase_;
    return offset < memory_.size() ? memory_[offset] : 0;
}

int16_t Spu::memRead16(uint32_t address) const {
    return int16_t(uint16_t(memRead8(address) | memRead8(address + 1) << 8));
}

uint32_t Spu::memRead32(uint32_t address) const {
    return uint32_t(uint16_t(memRead16(address))) | uint32_t(uint16_t(memRead16(address + 2))) << 16;
}

}  // namespace dsspu
