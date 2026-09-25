#include "dsspu/Adpcm.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace dsspu::adpcm {

const std::array<uint16_t, kMaxStepIndex + 1> kStepTable = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

const std::array<int8_t, 8> kIndexAdjust = {-1, -1, -1, -1, 2, 4, 6, 8};

void decodeNibble(State& state, uint8_t nibble) {
    const uint32_t step = kStepTable[size_t(state.index)];
    uint32_t diff = step >> 3;
    if (nibble & 0x1) diff += step >> 2;
    if (nibble & 0x2) diff += step >> 1;
    if (nibble & 0x4) diff += step;

    if (nibble & 0x8) {
        state.value = std::max(state.value - int32_t(diff), -0x7FFF);
    } else {
        state.value = std::min(state.value + int32_t(diff), 0x7FFF);
    }

    state.index = std::clamp(state.index + kIndexAdjust[nibble & 0x7], 0, kMaxStepIndex);
}

State readHeader(std::span<const uint8_t> data) {
    State state;
    if (data.size() < kHeaderBytes) return state;
    state.value = int16_t(uint16_t(data[0] | data[1] << 8));
    state.index = std::min<int32_t>(data[2] & 0x7F, kMaxStepIndex);
    return state;
}

Encoded encode(std::span<const int16_t> pcm) {
    Encoded out;
    out.sampleCount = pcm.size();

    State state;
    state.value = pcm.empty() ? 0 : std::clamp<int32_t>(pcm[0], -0x7FFF, 0x7FFF);
    state.index = 0;

    const auto header = uint16_t(int16_t(state.value));
    out.bytes = {uint8_t(header & 0xFF), uint8_t(header >> 8), uint8_t(state.index), 0};

    // Pad with repeats of the last sample so the decoder settles instead of
    // drifting, up to a whole word and at least 16 bytes.
    size_t nibbleCount = pcm.size();
    const size_t minNibbles = (16 - kHeaderBytes) * 2;
    nibbleCount = std::max(nibbleCount, minNibbles);
    nibbleCount = (nibbleCount + 7) / 8 * 8;

    for (size_t i = 0; i < nibbleCount; ++i) {
        const int32_t target = pcm.empty() ? 0 : pcm[std::min(i, pcm.size() - 1)];

        uint8_t best = 0;
        State bestState;
        int32_t bestError = std::numeric_limits<int32_t>::max();
        for (uint8_t nibble = 0; nibble < 16; ++nibble) {
            State trial = state;
            decodeNibble(trial, nibble);
            const int32_t error = std::abs(trial.value - target);
            if (error < bestError) {
                bestError = error;
                best = nibble;
                bestState = trial;
            }
        }
        state = bestState;

        if (i % 2 == 0) {
            out.bytes.push_back(best);
        } else {
            out.bytes.back() |= uint8_t(best << 4);
        }
    }
    return out;
}

std::vector<int16_t> decode(std::span<const uint8_t> data, size_t sampleCount) {
    std::vector<int16_t> out;
    out.reserve(sampleCount);
    State state = readHeader(data);
    for (size_t i = 0; i < sampleCount; ++i) {
        const size_t byteIndex = kHeaderBytes + i / 2;
        if (byteIndex >= data.size()) break;
        const uint8_t nibble = (i % 2 == 0) ? (data[byteIndex] & 0xF) : (data[byteIndex] >> 4);
        decodeNibble(state, nibble);
        out.push_back(int16_t(state.value));
    }
    return out;
}

}  // namespace dsspu::adpcm
