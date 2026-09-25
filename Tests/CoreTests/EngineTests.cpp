// Tests for the real-time engine and its lock-free queue.

#include "TestHarness.hpp"

#include <tracker/DemoSong.hpp>
#include <tracker/Engine.hpp>
#include <tracker/SpscQueue.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>
#include <vector>

// ---- Allocation counting ----------------------------------------------------
// Replaces global operator new for this test binary so tests can assert that a
// code path never touches the heap.

namespace {
std::atomic<bool> gCountAllocations{false};
std::atomic<size_t> gAllocationCount{0};

struct AllocationCounter {
    AllocationCounter() {
        gAllocationCount = 0;
        gCountAllocations = true;
    }
    ~AllocationCounter() { gCountAllocations = false; }
    size_t count() const { return gAllocationCount.load(); }
};
}  // namespace

void* operator new(size_t size) {
    if (gCountAllocations.load(std::memory_order_relaxed)) gAllocationCount.fetch_add(1);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

// ---- Helpers ----------------------------------------------------------------

namespace {

using tracker::DemoSong;
using tracker::Engine;

std::unique_ptr<Engine> makeDemoEngine() { return std::make_unique<Engine>(std::make_unique<DemoSong>()); }

// Renders `frames` in callbacks of `chunk` frames, like an audio device.
std::vector<float> renderLeft(Engine& engine, size_t frames, size_t chunk) {
    std::vector<float> left(frames);
    std::vector<float> right(frames);
    for (size_t done = 0; done < frames; done += chunk) {
        const size_t n = std::min(chunk, frames - done);
        engine.render(left.data() + done, right.data() + done, n);
    }
    return left;
}

}  // namespace

// ---- SpscQueue --------------------------------------------------------------

TEST("SpscQueue is FIFO and reports full and empty") {
    tracker::SpscQueue<int, 4> queue;  // holds 3
    int out = 0;
    CHECK(!queue.pop(out));
    CHECK(queue.push(1));
    CHECK(queue.push(2));
    CHECK(queue.push(3));
    CHECK(!queue.push(4));
    CHECK(queue.pop(out));
    CHECK_EQ(out, 1);
    CHECK(queue.push(4));
    for (int expected : {2, 3, 4}) {
        CHECK(queue.pop(out));
        CHECK_EQ(out, expected);
    }
    CHECK(!queue.pop(out));
}

TEST("SpscQueue delivers every item in order across threads") {
    tracker::SpscQueue<uint32_t, 64> queue;
    constexpr uint32_t kCount = 200'000;
    std::thread producer([&] {
        for (uint32_t i = 0; i < kCount; ++i) {
            while (!queue.push(i)) std::this_thread::yield();
        }
    });
    uint32_t expected = 0;
    bool inOrder = true;
    while (expected < kCount) {
        uint32_t value;
        if (queue.pop(value)) {
            inOrder &= value == expected;
            ++expected;
        }
    }
    producer.join();
    CHECK(inOrder);
}

// ---- Engine -----------------------------------------------------------------

TEST("Allocation counter sees heap allocations") {
    AllocationCounter counter;
    auto p = std::make_unique<std::vector<int>>(100);
    CHECK(counter.count() >= 2);
}

TEST("Engine render path never allocates") {
    auto engine = makeDemoEngine();
    engine->play();
    renderLeft(*engine, 256, 256);  // warm up

    std::vector<float> left(4096), right(4096);
    AllocationCounter counter;
    for (int i = 0; i < 500; ++i) {
        // Vary the callback size like real devices do, and mix in every
        // control-thread command.
        const size_t frames = size_t(64 + (i * 37) % 2048);
        engine->render(left.data(), right.data(), frames);
        if (i == 100) engine->setTickRate(90);
        if (i == 200) engine->stop();
        if (i == 250) engine->play();
        if (i == 300) engine->resetStats();
        (void)engine->status();
    }
    CHECK_EQ(counter.count(), size_t(0));
}

TEST("Engine output does not depend on the callback size") {
    auto a = makeDemoEngine();
    auto b = makeDemoEngine();
    a->play();
    b->play();
    const size_t frames = size_t(dsspu::kOutputSampleRate * 2);
    const auto outA = renderLeft(*a, frames, 1);
    const auto outB = renderLeft(*b, frames, 1000);
    CHECK(outA == outB);
}

TEST("Engine ticks at the configured tick rate") {
    auto engine = makeDemoEngine();
    engine->play();
    renderLeft(*engine, size_t(dsspu::kOutputSampleRate), 100);
    CHECK_EQ(engine->status().tick, uint32_t(60));

    engine->setTickRate(120);
    renderLeft(*engine, size_t(dsspu::kOutputSampleRate), 100);
    const uint32_t ticks = engine->status().tick;
    CHECK(ticks >= 179 && ticks <= 181);
}

TEST("Engine stop silences output on the next callback") {
    auto engine = makeDemoEngine();
    engine->play();
    renderLeft(*engine, 20000, 512);
    engine->stop();
    const auto out = renderLeft(*engine, 512, 512);
    bool silent = true;
    for (float s : out) silent &= s == 0.0f;
    CHECK(silent);
    CHECK(!engine->status().playing);
}

TEST("Engine status reports peaks and per-channel levels, then resets them") {
    auto engine = makeDemoEngine();
    engine->play();
    renderLeft(*engine, 512 * 128, 512);  // ~2 s in whole 512-frame callbacks

    const auto status = engine->status();
    CHECK(status.playing);
    CHECK(status.peakLeft > 0.1f && status.peakLeft <= 1.0f);
    CHECK(status.peakRight > 0.1f && status.peakRight <= 1.0f);
    // The demo uses channels 0 (bass), 1 (kick), 8-9 (PSG), 14-15 (noise).
    for (int ch : {0, 1, 8, 9, 14, 15}) CHECK(status.channelLevels[size_t(ch)] > 0.0f);
    for (int ch : {2, 3, 4, 5, 6, 7, 10, 11, 12, 13}) CHECK_EQ(status.channelLevels[size_t(ch)], 0.0f);
    CHECK_EQ(status.lastFrameCount, uint32_t(512));

    const auto again = engine->status();
    CHECK_EQ(again.peakLeft, 0.0f);
    CHECK_EQ(again.channelLevels[0], 0.0f);
}

TEST("Engine renders much faster than real time") {
    auto engine = makeDemoEngine();
    engine->play();
    const double seconds = 30;
    const auto start = std::chrono::steady_clock::now();
    renderLeft(*engine, size_t(dsspu::kOutputSampleRate * seconds), 256);
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double speed = seconds / elapsed;
    std::printf("        (rendered %.0f s of audio in %.3f s: %.0fx real time)\n", seconds, elapsed, speed);
    CHECK(speed > 20);
}
