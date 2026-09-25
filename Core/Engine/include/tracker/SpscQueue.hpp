#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace tracker {

// Fixed-size, lock-free single-producer/single-consumer queue. One thread may
// push and one other thread may pop. Neither side allocates or blocks, so it is
// safe to pop from the real-time audio thread.
template <typename T, size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::atomic<size_t>::is_always_lock_free);

public:
    // Producer only. Returns false if the queue is full.
    bool push(const T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next = (tail + 1) & (Capacity - 1);
        if (next == head_.load(std::memory_order_acquire)) return false;
        items_[tail] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer only. Returns false if the queue is empty.
    bool pop(T& out) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;
        out = items_[head];
        head_.store((head + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> items_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace tracker
