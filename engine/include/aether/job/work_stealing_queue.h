#pragma once

#include "aether/core/base.h"

#include <array>
#include <atomic>

namespace aether {

// Chase-Lev lock-free work-stealing deque. The owning thread pushes and pops
// from the bottom (LIFO — cheap, and keeps depth-first fork-join work cache-
// hot on the thread that spawned it); other threads steal from the top
// (FIFO) via a CAS loop when they run out of their own work. Capacity is
// fixed for the deque's lifetime; Push reports a full deque instead of
// dropping work, and the scheduler spills the overflow to a secondary queue
// that idle threads can also steal from.
template <typename T, usize Capacity>
class WorkStealingQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::atomic<T>::is_always_lock_free, "T must be lock-free atomic (e.g. a pointer)");

public:
    WorkStealingQueue() = default;

    WorkStealingQueue(const WorkStealingQueue&) = delete;
    WorkStealingQueue& operator=(const WorkStealingQueue&) = delete;

    // Owner-thread only. Returns false when the deque is full; the caller
    // decides where the item goes next (the scheduler spills it to overflow).
    bool Push(T item) {
        const i64 b = bottom_.load(std::memory_order_relaxed);
        // top_ only moves away from bottom_, so a stale read can report "full"
        // but never "space" when there is none. Reporting full early is safe.
        if (b - top_.load(std::memory_order_acquire) >= static_cast<i64>(Capacity)) {
            return false;
        }
        // Publish the item itself before making it reachable through bottom_:
        // the release store is what a thief's acquire load in Steal reads, so
        // every write to the job (function, data, counter, owner) is visible
        // before it runs the job.
        buffer_[static_cast<usize>(b) & kMask].store(item, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
        return true;
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

        // Acquire pairs with the release store in Push, so the job's fields are
        // visible before Execute reads them.
        out = buffer_[static_cast<usize>(t) & kMask].load(std::memory_order_acquire);
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
