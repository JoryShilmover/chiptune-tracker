#include "tracker/Engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace tracker {

using namespace dsspu;

namespace {

// Raises `target` to at least `value` without locking.
template <typename T>
void atomicMax(std::atomic<T>& target, T value) {
    T current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

}  // namespace

Engine::Engine(std::unique_ptr<Song> song) : song_(std::move(song)) {
    spu_.setMemory(song_->sampleMemory(), kSampleMemoryBase);
}

// ---- Control thread ---------------------------------------------------------

void Engine::play() { commands_.push({CommandType::Play, 0}); }

void Engine::stop() { commands_.push({CommandType::Stop, 0}); }

void Engine::setTickRate(double hz) { commands_.push({CommandType::SetTickRate, std::clamp(hz, 15.0, 240.0)}); }

void Engine::resetStats() { commands_.push({CommandType::ResetStats, 0}); }

EngineStatus Engine::status() {
    EngineStatus s;
    s.playing = pubPlaying_.load(std::memory_order_relaxed);
    s.tick = pubTick_.load(std::memory_order_relaxed);
    s.step = s.tick / std::max<uint32_t>(song_->ticksPerStep(), 1);
    s.tickRate = pubTickRate_.load(std::memory_order_relaxed);
    s.peakLeft = pubPeakLeft_.exchange(0, std::memory_order_relaxed);
    s.peakRight = pubPeakRight_.exchange(0, std::memory_order_relaxed);
    for (size_t i = 0; i < s.channelLevels.size(); ++i) {
        s.channelLevels[i] = pubChannelLevels_[i].exchange(0, std::memory_order_relaxed);
    }
    s.renderCalls = pubRenderCalls_.load(std::memory_order_relaxed);
    s.framesRendered = pubFramesRendered_.load(std::memory_order_relaxed);
    s.lastFrameCount = pubLastFrameCount_.load(std::memory_order_relaxed);
    s.lastRenderMicros = pubLastRenderMicros_.load(std::memory_order_relaxed);
    s.maxRenderMicros = pubMaxRenderMicros_.load(std::memory_order_relaxed);
    return s;
}

// ---- Audio thread -----------------------------------------------------------

void Engine::applyPendingCommands() {
    Command command;
    while (commands_.pop(command)) {
        switch (command.type) {
            case CommandType::Play:
                spu_.reset();
                song_->reset(spu_);
                tick_ = 0;
                framesUntilTick_ = 0;
                playing_ = true;
                break;
            case CommandType::Stop:
                playing_ = false;
                tick_ = 0;
                silenceAllChannels();
                break;
            case CommandType::SetTickRate:
                tickRate_ = command.value;
                break;
            case CommandType::ResetStats:
                maxRenderMicros_ = 0;
                pubMaxRenderMicros_.store(0, std::memory_order_relaxed);
                break;
        }
    }
    pubPlaying_.store(playing_, std::memory_order_relaxed);
    pubTickRate_.store(tickRate_, std::memory_order_relaxed);
}

void Engine::silenceAllChannels() {
    for (int ch = 0; ch < kChannelCount; ++ch) spu_.write32(channelRegister(ch, kChCnt), 0);
}

void Engine::render(float* left, float* right, size_t frames) {
    const auto start = std::chrono::steady_clock::now();
    applyPendingCommands();

    constexpr float kScale = 1.0f / 32768.0f;
    size_t done = 0;
    while (done < frames) {
        const size_t chunk = std::min(frames - done, kMaxFramesPerChunk);
        renderInterleaved(scratch_.data(), chunk);
        for (size_t i = 0; i < chunk; ++i) {
            left[done + i] = float(scratch_[i * 2]) * kScale;
            right[done + i] = float(scratch_[i * 2 + 1]) * kScale;
        }
        publish(scratch_.data(), chunk);
        done += chunk;
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double micros = std::chrono::duration<double, std::micro>(elapsed).count();
    maxRenderMicros_ = std::max(maxRenderMicros_, micros);
    ++renderCalls_;
    framesRendered_ += frames;
    pubLastRenderMicros_.store(micros, std::memory_order_relaxed);
    pubMaxRenderMicros_.store(maxRenderMicros_, std::memory_order_relaxed);
    pubLastFrameCount_.store(uint32_t(frames), std::memory_order_relaxed);
    pubRenderCalls_.store(renderCalls_, std::memory_order_relaxed);
    pubFramesRendered_.store(framesRendered_, std::memory_order_relaxed);
}

void Engine::render(int16_t* interleavedStereo, size_t frames) {
    applyPendingCommands();
    size_t done = 0;
    while (done < frames) {
        const size_t chunk = std::min(frames - done, kMaxFramesPerChunk);
        renderInterleaved(interleavedStereo + done * 2, chunk);
        publish(interleavedStereo + done * 2, chunk);
        done += chunk;
    }
}

// Runs song ticks at their exact positions within the buffer.
void Engine::renderInterleaved(int16_t* out, size_t frames) {
    const double framesPerTick = kOutputSampleRate / tickRate_;
    size_t done = 0;
    while (done < frames) {
        if (playing_ && framesUntilTick_ <= 0) {
            song_->tick(spu_, tick_++);
            framesUntilTick_ += framesPerTick;
        }
        size_t n = frames - done;
        if (playing_) n = std::min(n, size_t(std::max(1.0, std::ceil(framesUntilTick_))));
        spu_.render(out + done * 2, n);
        if (playing_) framesUntilTick_ -= double(n);
        done += n;
    }
    pubTick_.store(tick_, std::memory_order_relaxed);
}

void Engine::publish(const int16_t* interleaved, size_t frames) {
    int peakL = 0;
    int peakR = 0;
    for (size_t i = 0; i < frames; ++i) {
        peakL = std::max(peakL, std::abs(int(interleaved[i * 2])));
        peakR = std::max(peakR, std::abs(int(interleaved[i * 2 + 1])));
    }
    atomicMax(pubPeakLeft_, float(peakL) / 32768.0f);
    atomicMax(pubPeakRight_, float(peakR) / 32768.0f);

    const auto& levels = spu_.channelPeaks();
    for (size_t i = 0; i < levels.size(); ++i) {
        atomicMax(pubChannelLevels_[i], float(levels[i]) / 32767.0f);
    }
    spu_.resetChannelPeaks();
}

}  // namespace tracker
