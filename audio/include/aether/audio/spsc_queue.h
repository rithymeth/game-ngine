#pragma once

#include "aether/core/base.h"

#include <atomic>
#include <memory>

namespace aether::audio {

// A fixed-capacity single-producer, single-consumer queue without locks
// (Phase 17 step 5): one thread pushes, one other thread pops. Capacity is
// rounded up to a power of two. T should be cheap to copy (pointers, ids).
template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(usize capacity = 1024) {
        usize c = 2;
        while (c < capacity) c <<= 1;
        mask_ = c - 1;
        items_ = std::make_unique<T[]>(c);
    }

    // Producer only. False when full.
    bool Push(const T& item) {
        const usize head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) > mask_) return false;
        items_[head & mask_] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. False when empty.
    bool Pop(T& out) {
        const usize tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = items_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    usize Capacity() const { return mask_ + 1; }
    // Approximate from either side (exact when the other side is idle).
    usize Size() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }

private:
    usize mask_ = 0;
    std::unique_ptr<T[]> items_;
    alignas(64) std::atomic<usize> head_{0}; // written by the producer
    alignas(64) std::atomic<usize> tail_{0}; // written by the consumer
};

} // namespace aether::audio
