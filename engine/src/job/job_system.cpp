#include "aether/job/job_system.h"

#include "aether/core/profiler.h"

#include <string>

#include "aether/core/log.h"

namespace aether {

thread_local u32 JobSystem::tls_thread_index_ = ~0u;

namespace {
constexpr u32 kInvalidThreadIndex = ~0u;
}

JobSystem::JobSystem(u32 worker_count) {
    if (worker_count == 0) {
        u32 hw = std::thread::hardware_concurrency();
        worker_count = (hw > 1) ? hw - 1 : 1;
    }

    thread_contexts_.reserve(worker_count + 1);
    for (u32 i = 0; i <= worker_count; ++i) {
        thread_contexts_.push_back(std::make_unique<ThreadContext>());
    }

    // The constructing thread participates as worker 0.
    tls_thread_index_ = 0;

    workers_.reserve(worker_count);
    for (u32 i = 1; i <= worker_count; ++i) {
        workers_.emplace_back([this, i] { WorkerMain(i); });
    }

    AETHER_LOG_INFO("JobSystem", "Initialized with %u worker thread(s) + calling thread", worker_count);
}

JobSystem::~JobSystem() {
    running_.store(false, std::memory_order_release);
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

JobSystem::Job* JobSystem::AllocateJob(u32 thread_index, JobFunc fn, void* data, JobCounter& counter) {
    ThreadContext& ctx = *thread_contexts_[thread_index];
    // Retired slots may be reused only when every job this thread handed out
    // has finished. in_flight covers queued, overflowed and running jobs, so a
    // pointer another thread still holds keeps its slot untouched. New slots
    // are appended; std::deque never moves the existing ones.
    if (ctx.in_flight.load(std::memory_order_acquire) == 0) {
        ctx.next_slot = 0;
    }
    Job* job = ctx.next_slot < ctx.jobs.size() ? &ctx.jobs[ctx.next_slot] : &ctx.jobs.emplace_back();
    ++ctx.next_slot;
    job->function = fn;
    job->data = data;
    job->counter = &counter;
    job->owner = &ctx;
    ctx.in_flight.fetch_add(1, std::memory_order_relaxed);
    return job;
}

void JobSystem::Schedule(JobFunc fn, void* data, JobCounter& counter) {
    AETHER_ASSERT(tls_thread_index_ != kInvalidThreadIndex);
    counter.fetch_add(1, std::memory_order_relaxed);
    ThreadContext& ctx = *thread_contexts_[tls_thread_index_];
    Job* job = AllocateJob(tls_thread_index_, fn, data, counter);
    if (!ctx.queue.Push(job)) ctx.PushOverflow(job);
}

void JobSystem::ScheduleBatch(const JobDecl* jobs, u32 count, JobCounter& counter) {
    AETHER_ASSERT(tls_thread_index_ != kInvalidThreadIndex);
    if (count == 0) {
        return;
    }
    counter.fetch_add(static_cast<i32>(count), std::memory_order_relaxed);
    ThreadContext& ctx = *thread_contexts_[tls_thread_index_];
    for (u32 i = 0; i < count; ++i) {
        Job* job = AllocateJob(tls_thread_index_, jobs[i].function, jobs[i].data, counter);
        if (!ctx.queue.Push(job)) ctx.PushOverflow(job);
    }
}

void JobSystem::Execute(Job* job) {
    // owner is read before the counter drops: the caller may destroy its
    // counter as soon as it reaches zero, and the job storage stays alive
    // until in_flight is decremented below.
    ThreadContext* owner = job->owner;
    job->function(job->data);
    job->counter->fetch_sub(1, std::memory_order_acq_rel);
    owner->in_flight.fetch_sub(1, std::memory_order_acq_rel);
}

bool JobSystem::TryRunOneJob(u32 thread_index) {
    Job* job = nullptr;
    ThreadContext& own = *thread_contexts_[thread_index];
    if (own.queue.Pop(job) || own.TryPopOverflow(job)) {
        Execute(job);
        return true;
    }

    u32 n = static_cast<u32>(thread_contexts_.size());
    if (n <= 1) {
        return false;
    }

    u32 start = own.steal_start++ % n;
    for (u32 i = 0; i < n; ++i) {
        u32 victim = (start + i) % n;
        if (victim == thread_index) {
            continue;
        }
        ThreadContext& other = *thread_contexts_[victim];
        if (other.queue.Steal(job) || other.TryPopOverflow(job)) {
            Execute(job);
            return true;
        }
    }
    return false;
}

void JobSystem::Wait(JobCounter& counter) {
    AETHER_ASSERT(tls_thread_index_ != kInvalidThreadIndex);
    while (counter.load(std::memory_order_acquire) > 0) {
        if (!TryRunOneJob(tls_thread_index_)) {
            std::this_thread::yield();
        }
    }
}

void JobSystem::WorkerMain(u32 thread_index) {
    tls_thread_index_ = thread_index;
    Profiler::Get().SetThreadName("Worker " + std::to_string(thread_index));
    while (running_.load(std::memory_order_acquire)) {
        if (!TryRunOneJob(thread_index)) {
            std::this_thread::yield();
        }
    }
}

} // namespace aether
