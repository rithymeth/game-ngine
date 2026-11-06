#pragma once

// Phase 38 step 8: the render thread -- a queue of work drained in order on one worker.
//
// Recording GPU work (creating a pipeline, uploading a texture, submitting a frame's draws) does not have
// to happen on the thread that runs the game. A caller posts work and carries on; Flush is where it waits,
// which is the frame boundary.
//
// The work is a std::function rather than a draw item, because everything the renderer must keep from
// interleaving -- submissions, uploads, pipeline creation -- goes through the same queue. That also makes
// it testable with no GPU: a test posts lambdas and checks they ran, in order, on another thread.

#include "aether/core/base.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace aether::gfx {

class RenderThread {
public:
    RenderThread() : thread_([this] { Run(); }) {}

    ~RenderThread() { Stop(); }

    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    // Queues `work` to run on the render thread. False once the thread has been stopped, so a caller that
    // is shutting down finds out rather than posting into nothing.
    bool Post(std::function<void()> work) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return false;
            queue_.push_back(std::move(work));
        }
        ready_.notify_one();
        return true;
    }

    // Blocks until everything posted before this call has run. Work posted after it may still be pending,
    // which is what makes this a frame boundary rather than a shutdown. Implemented by posting a marker
    // and waiting for it, so it needs no separate bookkeeping and cannot get the count wrong.
    void Flush() {
        auto marker = std::make_shared<std::promise<void>>();
        std::future<void> done = marker->get_future();
        if (!Post([marker] { marker->set_value(); })) return; // already stopped: nothing to wait for
        done.wait();
    }

    // Drains the queue, stops the thread and joins it. Safe to call more than once.
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            stopping_ = true;
        }
        ready_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    // Work posted but not yet run.
    usize Pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    // True inside a posted command. Lets a caller assert where its work is running, and is what a test
    // uses to show the thread is doing the work rather than the caller.
    bool OnRenderThread() const { return std::this_thread::get_id() == thread_.get_id(); }

private:
    void Run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stopping_) return; // stopped, and the queue is drained
                continue;
            }
            std::function<void()> work = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            if (work) work();
            lock.lock();
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> queue_;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace aether::gfx