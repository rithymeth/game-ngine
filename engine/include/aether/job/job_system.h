#pragma once

#include "aether/core/base.h"
#include "aether/job/work_stealing_queue.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace aether {

using JobFunc = void (*)(void* data);

// Shared completion counter for a batch of jobs: Schedule()/ScheduleBatch()
// increment it, each job decrements it on completion, and Wait() blocks
// (while helping run other jobs) until it hits zero. The caller owns the
// counter's storage, so fork-join composes naturally:
//
//   JobCounter counter{0};
//   job_system.ScheduleBatch(jobs, count, counter);
//   job_system.Wait(counter);
//
// This is the explicit dependency primitive the Task Graph builds on;
// automatic read/write dependency inference from component access is a
// scheduling layer on top of this, not part of it.
// Keep the counter and every job's data alive until Wait(counter) returns.
using JobCounter = std::atomic<i32>;

struct JobDecl {
    JobFunc function = nullptr;
    void* data = nullptr;
};

// Work-stealing job scheduler: every participating thread (the thread that
// constructs the JobSystem, plus its worker threads) owns a lock-free deque
// of pending jobs. A thread runs its own work first and steals from others
// only when idle, which is what lets this scale near-linearly with core
// count for the frame's Physics/Animation/AI fan-out. A batch larger than the
// deque spills into that thread's overflow list, which idle workers also
// steal from, so fan-out is not capped by the deque capacity and no job is
// dropped.
// Schedule, ScheduleBatch and Wait require the constructing thread or one of
// this JobSystem's workers; external threads are not registered. Wait for all
// submitted jobs before destroying the JobSystem.
class JobSystem {
public:
    // worker_count == 0 picks hardware_concurrency() - 1, since the
    // constructing thread (typically the main thread) participates as
    // worker 0 rather than sitting idle in Wait().
    explicit JobSystem(u32 worker_count = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    void Schedule(JobFunc fn, void* data, JobCounter& counter);
    void ScheduleBatch(const JobDecl* jobs, u32 count, JobCounter& counter);

    // Helps execute pending jobs (this thread's queue, then stealing from
    // others) until `counter` reaches zero. Safe to call recursively from
    // inside a running job to express nested fork-join.
    void Wait(JobCounter& counter);

    u32 ThreadCount() const { return static_cast<u32>(thread_contexts_.size()); }

private:
    static constexpr usize kQueueCapacity = 4096;

    struct ThreadContext;

    struct Job {
        JobFunc function = nullptr;
        void* data = nullptr;
        JobCounter* counter = nullptr;
        ThreadContext* owner = nullptr;
    };

    struct ThreadContext {
        WorkStealingQueue<Job*, kQueueCapacity> queue;

        // Job storage owned by this thread. std::deque keeps Job addresses
        // stable as it grows, and retired slots are reused only once
        // in_flight reaches zero: a job that is queued, overflowed, or running
        // still counts there, so no live pointer can be overwritten.
        std::deque<Job> jobs;
        usize next_slot = 0; // owner-thread only
        std::atomic<usize> in_flight{0};

        // A batch larger than the deque spills here instead of being dropped.
        // Only the owning thread pushes; any thread may steal, so a large
        // fan-out still runs in parallel instead of serially on one thread.
        std::mutex overflow_mutex;
        std::deque<Job*> overflow;

        // Owner-thread-only rotating start for victim selection, so idle workers
        // don't all contend on one shared steal counter.
        u32 steal_start = 0;

        void PushOverflow(Job* job) {
            std::lock_guard<std::mutex> lock(overflow_mutex);
            overflow.push_back(job);
        }

        bool TryPopOverflow(Job*& job) {
            std::lock_guard<std::mutex> lock(overflow_mutex);
            if (overflow.empty()) return false;
            job = overflow.front();
            overflow.pop_front();
            return true;
        }
    };

    Job* AllocateJob(u32 thread_index, JobFunc fn, void* data, JobCounter& counter);
    bool TryRunOneJob(u32 thread_index);
    static void Execute(Job* job);
    void WorkerMain(u32 thread_index);

    std::vector<std::unique_ptr<ThreadContext>> thread_contexts_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{true};

    static thread_local u32 tls_thread_index_;
};

} // namespace aether
