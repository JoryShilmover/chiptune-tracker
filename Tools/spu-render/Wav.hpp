#pragma once

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

// Writes 16-bit stereo PCM as a WAV file. Returns false on I/O failure.
inline bool writeWav(const std::string& path, std::span<const int16_t> interleavedStereo, uint32_t sampleRate) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;

    auto u32 = [file](uint32_t v) {
        const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        std::fwrite(b, 1, 4, file);
    };
    auto u16 = [file](uint16_t v) {
        const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
        std::fwrite(b, 1, 2, file);
    };

    constexpr uint16_t kChannels = 2;
    constexpr uint16_t kBitsPerSample = 16;
    const auto dataBytes = uint32_t(interleavedStereo.size() * sizeof(int16_t));

    std::fwrite("RIFF", 1, 4, file);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, file);
    u32(16);
    u16(1);  // PCM
    u16(kChannels);
    u32(sampleRate);
    u32(sampleRate * kChannels * kBitsPerSample / 8);
    u16(kChannels * kBitsPerSample / 8);
    u16(kBitsPerSample);
    std::fwrite("data", 1, 4, file);
    u32(dataBytes);
    for (int16_t s : interleavedStereo) u16(uint16_t(s));

    return std::fclose(file) == 0;
}
