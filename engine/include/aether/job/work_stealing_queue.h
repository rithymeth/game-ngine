#pragma once

#include "aether/core/base.h"

#include <array>
#include <atomic>

namespace aether {

// Chase-Lev lock-free work-stealing deque. The owning thread pushes and pops
// from the bottom (LIFO — cheap, and keeps depth-first fork-join work cache-
// hot on the thread that spawned it); other threads steal from the top
// (FIFO) via a CAS loop when they run out of their own work. Capacity is
// fixed for the deque's lifetime; callers size it generously and treat
// overflow (silently dropped Push, in a debug build an assert) as a bug in
// how much work is being fanned out per frame, not something to recover from
// at runtime.
template <typename T, usize Capacity>
class WorkStealingQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::atomic<T>::is_always_lock_free, "T must be lock-free atomic (e.g. a pointer)");

public:
    WorkStealingQueue() = default;

    WorkStealingQueue(const WorkStealingQueue&) = delete;
    WorkStealingQueue& operator=(const WorkStealingQueue&) = delete;

    // Owner-thread only.
    void Push(T item) {
        i64 b = bottom_.load(std::memory_order_relaxed);
        AETHER_ASSERT(b - top_.load(std::memory_order_relaxed) < static_cast<i64>(Capacity));
        buffer_[static_cast<usize>(b) & kMask].store(item, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
    }

    // Owner-thread only. Returns false if the deque is empty.
    bool Pop(T& out) {
        i64 b = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        i64 t = top_.load(std::memory_order_relaxed);

        if (t > b) {
            bottom_.store(b + 1, std::memory_order_relaxed); // deque was empty; restore
            return false;
        }

        out = buffer_[static_cast<usize>(b) & kMask].load(std::memory_order_relaxed);
        if (t == b) {
            // Last item in the deque: race against concurrent thieves for it.
            if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                bottom_.store(b + 1, std::memory_order_relaxed); // lost the race
                return false;
            }
            bottom_.store(b + 1, std::memory_order_relaxed);
        }
        return true;
    }

    // Any thread. Returns false if the deque is empty or a steal raced and lost.
    bool Steal(T& out) {
        i64 t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        i64 b = bottom_.load(std::memory_order_acquire);

        if (t >= b) {
            return false;
        }

        out = buffer_[static_cast<usize>(t) & kMask].load(std::memory_order_relaxed);
        return top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
    }

    bool Empty() const {
        i64 b = bottom_.load(std::memory_order_relaxed);
        i64 t = top_.load(std::memory_order_relaxed);
        return t >= b;
    }

private:
    static constexpr usize kMask = Capacity - 1;

    std::array<std::atomic<T>, Capacity> buffer_{};
    alignas(kCacheLineSize) std::atomic<i64> top_{0};
    alignas(kCacheLineSize) std::atomic<i64> bottom_{0};
};

} // namespace aether
