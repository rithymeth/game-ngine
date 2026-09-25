#pragma once

#include "aether/core/base.h"
#include "aether/job/work_stealing_queue.h"

#include <array>
#include <atomic>
#include <memory>
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
using JobCounter = std::atomic<i32>;

struct JobDecl {
    JobFunc function = nullptr;
    void* data = nullptr;
};

// Work-stealing job scheduler: every participating thread (the thread that
// constructs the JobSystem, plus its worker threads) owns a lock-free deque
// of pending jobs. A thread runs its own work first and steals from others
// only when idle, which is what lets this scale near-linearly with core
// count for the frame's Physics/Animation/AI fan-out.
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
    static constexpr usize kJobPoolSize = 4096;

    struct Job {
        JobFunc function = nullptr;
        void* data = nullptr;
        JobCounter* counter = nullptr;
    };

    struct ThreadContext {
        WorkStealingQueue<Job*, kQueueCapacity> queue;
        std::array<Job, kJobPoolSize> job_pool;
        usize next_job_slot = 0;
    };

    Job* AllocateJob(u32 thread_index, JobFunc fn, void* data, JobCounter& counter);
    bool TryRunOneJob(u32 thread_index);
    static void Execute(Job* job);
    void WorkerMain(u32 thread_index);

    std::vector<std::unique_ptr<ThreadContext>> thread_contexts_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{true};
    std::atomic<u32> steal_cursor_{0};

    static thread_local u32 tls_thread_index_;
};

} // namespace aether
