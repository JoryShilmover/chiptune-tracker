#pragma once

// IMA-ADPCM as implemented by the DS sound hardware.
//
// A DS ADPCM sample starts with a 4-byte header (initial value as int16,
// initial step index as uint8, one unused byte), followed by 4-bit nibbles,
// low nibble first. The DS differs from textbook IMA-ADPCM in its clamping:
// decoded values are limited to -0x7FFF..+0x7FFF, not -0x8000.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dsspu::adpcm {

inline constexpr size_t kHeaderBytes = 4;
inline constexpr int kMaxStepIndex = 88;

extern const std::array<uint16_t, kMaxStepIndex + 1> kStepTable;
extern const std::array<int8_t, 8> kIndexAdjust;

struct State {
    int32_t value = 0;
    int32_t index = 0;
};

// Applies one nibble to the decoder state, exactly as the hardware does.
void decodeNibble(State& state, uint8_t nibble);

// Reads the 4-byte header at the start of `data`.
State readHeader(std::span<const uint8_t> data);

struct Encoded {
    // Header plus nibbles, padded to a multiple of 4 bytes and to at least
    // 16 bytes (the hardware plays shorter samples as silence).
    std::vector<uint8_t> bytes;
    size_t sampleCount = 0;
};

// Encodes 16-bit PCM. For each sample it picks the nibble whose decoded result
// is closest to the input, using the same decoder the hardware uses, so the
// encoder's prediction can never drift from what the DS will play.
Encoded encode(std::span<const int16_t> pcm);

// Decodes `sampleCount` samples (header included in `data`).
std::vector<int16_t> decode(std::span<const uint8_t> data, size_t sampleCount);

}  // namespace dsspu::adpcm
