#pragma once

#include "aether/job/job_system.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>

namespace aether {

// Adapts aether::JobSystem (the Phase 2 work-stealing scheduler) to Jolt's
// JPH::JobSystem interface, so Jolt Physics runs its jobs on the same worker
// threads as the rest of the engine instead of spinning up its own thread
// pool. JobSystemWithBarrier supplies the Barrier/WaitForJobs machinery that
// JPH::PhysicsSystem::Update() blocks on (it executes barrier-owned jobs
// directly on the calling thread while waiting, in addition to whatever
// finishes on aether::JobSystem's workers) — this adapter only needs to
// implement job allocation and getting a job onto a queue.
//
// Anything that ends up calling into this adapter's QueueJob — in practice,
// anywhere JPH::PhysicsSystem::Update() (i.e. PhysicsWorld::Step()) is
// called from — must run on a thread already registered with the
// aether::JobSystem passed to the constructor (the thread that constructed
// it, or one of its own workers), since Schedule() asserts that. Job
// continuations queued from inside a running Jolt job satisfy this
// automatically, since they execute on an aether::JobSystem worker thread.
class JoltJobSystemAdapter final : public JPH::JobSystemWithBarrier {
public:
    // Note: `aether::` on the parameter type below is not decorative — inside
    // this class, unqualified `JobSystem` resolves to the base class
    // `JPH::JobSystem` via injected-class-name lookup, shadowing
    // `aether::JobSystem`. Every use of our own JobSystem type in this class
    // must be explicitly qualified.
    JoltJobSystemAdapter(aether::JobSystem& job_system, JPH::uint max_jobs, JPH::uint max_barriers);
    ~JoltJobSystemAdapter() override;

    int GetMaxConcurrency() const override;

    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& job_function,
                         JPH::uint32 num_dependencies = 0) override;

protected:
    void QueueJob(Job* job) override;
    void QueueJobs(Job** jobs, JPH::uint num_jobs) override;
    void FreeJob(Job* job) override;

private:
    aether::JobSystem& job_system_;
    JPH::FixedSizeFreeList<Job> jobs_;

    // Jolt's own Barrier (via JobHandle::IsDone()) tracks each job's real
    // completion. This counter counts the scheduled wrappers, which finish
    // after the job's Release(); the destructor waits on it so no worker
    // still holds a job when the pool is destroyed.
    JobCounter dummy_counter_{0};
};

} // namespace aether
