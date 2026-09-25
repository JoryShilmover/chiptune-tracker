#pragma once

#include "tracker/Song.hpp"
#include "tracker/SpscQueue.hpp"

#include <dsspu/Spu.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace tracker {

struct EngineStatus {
    bool playing = false;
    uint32_t tick = 0;
    uint32_t step = 0;
    double tickRate = 0;
    float peakLeft = 0;   // 0-1, loudest sample since the last status read
    float peakRight = 0;
    std::array<float, dsspu::kChannelCount> channelLevels{};  // 0-1, same window
    uint64_t renderCalls = 0;
    uint64_t framesRendered = 0;
    uint32_t lastFrameCount = 0;
    double lastRenderMicros = 0;
    double maxRenderMicros = 0;
};

// Owns the SPU and a song, and renders audio in real time.
//
// Threading: one control thread (the UI) calls play/stop/setTickRate/
// resetStats and status(); one audio thread calls render(). Commands cross
// threads through a lock-free queue and telemetry through atomics, so render()
// never allocates, locks or blocks.
class Engine {
public:
    static constexpr size_t kMaxFramesPerChunk = 1024;
    static constexpr double kDefaultTickRate = 60.0;

    explicit Engine(std::unique_ptr<Song> song);

    // --- Control thread ---
    void play();
    void stop();
    void setTickRate(double hz);  // clamped to 15-240 Hz
    void resetStats();
    // Call before rendering starts. On by default, like a DS or DS Lite.
    void setDegradeTo10Bit(bool enabled) { spu_.setDegradeTo10Bit(enabled); }
    // Also resets the peak meters, so poll it from a single thread.
    EngineStatus status();

    // --- Audio thread ---
    // Renders deinterleaved float stereo at dsspu::kOutputSampleRate.
    void render(float* left, float* right, size_t frames);
    // Renders interleaved 16-bit stereo (for offline rendering).
    void render(int16_t* interleavedStereo, size_t frames);

    // For offline rendering and tests: applies pending commands now.
    void applyPendingCommands();

private:
    enum class CommandType : uint8_t { Play, Stop, SetTickRate, ResetStats };
    struct Command {
        CommandType type = CommandType::Stop;
        double value = 0;
    };

    void renderInterleaved(int16_t* out, size_t frames);
    void silenceAllChannels();
    void publish(const int16_t* interleaved, size_t frames);

    dsspu::Spu spu_;
    std::unique_ptr<Song> song_;
    SpscQueue<Command, 64> commands_;
    std::array<int16_t, kMaxFramesPerChunk * 2> scratch_{};

    // Audio-thread state.
    bool playing_ = false;
    uint32_t tick_ = 0;
    double tickRate_ = kDefaultTickRate;
    double framesUntilTick_ = 0;
    double maxRenderMicros_ = 0;
    uint64_t renderCalls_ = 0;
    uint64_t framesRendered_ = 0;

    // Telemetry, written by the audio thread and read by the control thread.
    std::atomic<bool> pubPlaying_{false};
    std::atomic<uint32_t> pubTick_{0};
    std::atomic<double> pubTickRate_{kDefaultTickRate};
    std::atomic<float> pubPeakLeft_{0};
    std::atomic<float> pubPeakRight_{0};
    std::array<std::atomic<float>, dsspu::kChannelCount> pubChannelLevels_{};
    std::atomic<uint64_t> pubRenderCalls_{0};
    std::atomic<uint64_t> pubFramesRendered_{0};
    std::atomic<uint32_t> pubLastFrameCount_{0};
    std::atomic<double> pubLastRenderMicros_{0};
    std::atomic<double> pubMaxRenderMicros_{0};

    static_assert(std::atomic<double>::is_always_lock_free);
    static_assert(std::atomic<float>::is_always_lock_free);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
};

}  // namespace tracker
