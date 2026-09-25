#pragma once

// Nintendo DS sound hardware (SPU) emulator.
//
// Written from the GBATEK hardware documentation. Behavior that GBATEK leaves
// ambiguous was checked against melonDS by observation only; no melonDS code
// is used (melonDS is GPL, this core is not).
//
// The SPU is driven exactly like the real hardware: through writes to its
// memory-mapped registers (0x04000400-0x0400051F). Anything the tracker does
// must go through these registers, which is what guarantees songs can play
// back on a real DS.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dsspu {

// ---- Clocks -----------------------------------------------------------------

inline constexpr uint32_t kArm7ClockHz = 33'513'982;
// Channel timers tick at half the ARM7 clock.
inline constexpr uint32_t kTimerClockHz = kArm7ClockHz / 2;
// The mixer produces one output sample every 1024 ARM7 cycles.
inline constexpr uint32_t kCyclesPerOutputSample = 1024;
inline constexpr uint32_t kTimerTicksPerOutputSample = kCyclesPerOutputSample / 2;
// ~32,728.5 Hz.
inline constexpr double kOutputSampleRate = double(kArm7ClockHz) / kCyclesPerOutputSample;

inline constexpr int kChannelCount = 16;
// PSG square waves are only available on channels 8-13, noise on 14-15.
inline constexpr int kFirstPsgChannel = 8;
inline constexpr int kFirstNoiseChannel = 14;

// ---- Registers --------------------------------------------------------------

inline constexpr uint32_t kRegChannelBase = 0x04000400;
inline constexpr uint32_t kRegChannelStride = 0x10;
inline constexpr uint32_t kRegSoundCnt = 0x04000500;
inline constexpr uint32_t kRegSoundBias = 0x04000504;

// Offsets within a channel's register block.
inline constexpr uint32_t kChCnt = 0x0;  // SOUNDxCNT (32-bit)
inline constexpr uint32_t kChSad = 0x4;  // SOUNDxSAD source address (32-bit)
inline constexpr uint32_t kChTmr = 0x8;  // SOUNDxTMR timer reload (16-bit)
inline constexpr uint32_t kChPnt = 0xA;  // SOUNDxPNT loop start, in words (16-bit)
inline constexpr uint32_t kChLen = 0xC;  // SOUNDxLEN loop length, in words (32-bit, 21 bits used)

constexpr uint32_t channelRegister(int channel, uint32_t offset) {
    return kRegChannelBase + uint32_t(channel) * kRegChannelStride + offset;
}

enum class Format : uint8_t { Pcm8 = 0, Pcm16 = 1, Adpcm = 2, PsgNoise = 3 };

// Only bit 0 (loop) and bit 1 (stop at end) matter to the hardware. Manual (0)
// keeps reading past the end of the sample; 3 is documented as prohibited and
// behaves like Loop.
enum class RepeatMode : uint8_t { Manual = 0, Loop = 1, OneShot = 2 };

enum class VolumeDivider : uint8_t { Div1 = 0, Div2 = 1, Div4 = 2, Div16 = 3 };

// Builds a SOUNDxCNT value.
struct ChannelControl {
    uint8_t volume = 127;  // 0-127
    VolumeDivider divider = VolumeDivider::Div1;
    bool hold = false;
    uint8_t pan = 64;      // 0 = left, 64 = center, 127 = right
    uint8_t duty = 0;      // PSG only: 0-6 = (n+1)/8 high, 7 = always low
    RepeatMode repeat = RepeatMode::Loop;
    Format format = Format::Pcm8;
    bool start = true;

    constexpr uint32_t encode() const {
        return uint32_t(volume & 0x7F)
             | uint32_t(divider) << 8
             | uint32_t(hold) << 15
             | uint32_t(pan & 0x7F) << 16
             | uint32_t(duty & 0x7) << 24
             | uint32_t(repeat) << 27
             | uint32_t(format) << 29
             | uint32_t(start) << 31;
    }
};

// SOUNDCNT (0x04000500). Output routing and capture-mute bits are stored but not
// yet emulated; all channels are mixed to both outputs.
struct MasterControl {
    uint8_t volume = 127;  // 0-127
    bool enable = true;

    constexpr uint16_t encode() const {
        return uint16_t((volume & 0x7F) | (enable ? 0x8000 : 0));
    }
};

// Timer reload value that plays samples at `sampleRateHz`.
uint16_t timerForSampleRate(double sampleRateHz);
// Timer reload value for a PSG square wave at `frequencyHz` (8 steps per cycle).
uint16_t timerForPsgFrequency(double frequencyHz);
// The sample rate a timer reload value produces.
double sampleRateForTimer(uint16_t timer);

// ---- SPU --------------------------------------------------------------------

class Spu {
public:
    Spu();

    // Returns all registers and channel state to power-on values. Keeps the
    // memory mapping.
    void reset();

    // The memory channels read samples from. SOUNDxSAD is an address in this
    // region; on a DS it is usually main RAM (0x02000000). Reads outside the
    // region return 0. The memory must outlive the Spu.
    void setMemory(std::span<const uint8_t> memory, uint32_t baseAddress = 0x02000000);

    void write8(uint32_t address, uint8_t value);
    void write16(uint32_t address, uint16_t value);
    void write32(uint32_t address, uint32_t value);
    uint8_t read8(uint32_t address) const;
    uint16_t read16(uint32_t address) const;
    uint32_t read32(uint32_t address) const;

    // Convenience wrappers around the register writes.
    void writeChannelControl(int channel, const ChannelControl& control) {
        write32(channelRegister(channel, kChCnt), control.encode());
    }
    void writeMasterControl(const MasterControl& control) {
        write16(kRegSoundCnt, control.encode());
    }

    // SOUNDxCNT bit 31: set while a channel is playing.
    bool isChannelBusy(int channel) const;

    // The original DS and DS Lite output 10-bit audio. On by default; turn off
    // for a clean 16-bit reference render.
    void setDegradeTo10Bit(bool enabled) { degradeTo10Bit_ = enabled; }

    // Renders `frames` stereo frames (interleaved L, R) at kOutputSampleRate.
    void render(int16_t* interleavedStereo, size_t frames);

private:
    struct Channel {
        // Decoded from registers.
        uint32_t control = 0;
        uint32_t sourceAddress = 0;
        uint16_t timerReload = 0;
        uint32_t loopStartBytes = 0;
        uint32_t lengthBytes = 0;
        uint32_t volume = 0;        // 0-128 (127 is treated as 128)
        uint32_t volumeShift = 4;   // 16.4 fixed point: 4, 3, 2 or 0
        uint32_t pan = 0;           // 0-128 (127 is treated as 128)

        // Playback state.
        bool keyOn = false;
        bool running = false;
        uint32_t timer = 0;
        int32_t position = 0;
        int16_t sample = 0;
        uint16_t noiseLfsr = 0x7FFF;
        int32_t adpcmValue = 0;
        int32_t adpcmIndex = 0;
        int32_t adpcmLoopValue = 0;
        int32_t adpcmLoopIndex = 0;
        uint8_t adpcmByte = 0;

        Format format() const { return Format((control >> 29) & 0x3); }
        uint32_t repeat() const { return (control >> 27) & 0x3; }
        uint32_t duty() const { return (control >> 24) & 0x7; }
    };

    void onRegisterWrite(uint32_t offset, uint32_t size, uint32_t oldChannelControl);
    void decodeChannel(int index);
    void start(Channel& ch);
    void stop(Channel& ch, int index);
    int32_t runChannel(Channel& ch, int index);
    void nextSample(Channel& ch, int index);
    void nextPcm8(Channel& ch, int index);
    void nextPcm16(Channel& ch, int index);
    void nextAdpcm(Channel& ch, int index);
    void nextPsg(Channel& ch);
    void nextNoise(Channel& ch);
    uint8_t memRead8(uint32_t address) const;
    int16_t memRead16(uint32_t address) const;
    uint32_t memRead32(uint32_t address) const;

    // Raw register bytes for 0x04000400-0x0400051F.
    std::array<uint8_t, 0x120> registers_{};
    std::array<Channel, kChannelCount> channels_{};
    uint32_t masterVolume_ = 0;  // 0-128 (127 is treated as 128)
    bool masterEnable_ = false;
    uint32_t bias_ = 0;
    bool degradeTo10Bit_ = true;

    std::span<const uint8_t> memory_{};
    uint32_t memoryBase_ = 0x02000000;
};

}  // namespace dsspu
