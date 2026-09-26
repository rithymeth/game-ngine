#include "aether/physics/jolt_job_system_adapter.h"

#include "aether/core/log.h"

#include <chrono>
#include <thread>

namespace aether {

JoltJobSystemAdapter::JoltJobSystemAdapter(aether::JobSystem& job_system, JPH::uint max_jobs, JPH::uint max_barriers)
    : job_system_(job_system) {
    JobSystemWithBarrier::Init(max_barriers);
    jobs_.Init(max_jobs, max_jobs);
}

JoltJobSystemAdapter::~JoltJobSystemAdapter() = default;

int JoltJobSystemAdapter::GetMaxConcurrency() const {
    return static_cast<int>(job_system_.ThreadCount());
}

JoltJobSystemAdapter::JobHandle JoltJobSystemAdapter::CreateJob(const char* name, JPH::ColorArg color,
                                                                 const JobFunction& job_function,
                                                                 JPH::uint32 num_dependencies) {
    JPH::uint32 index;
    for (;;) {
        index = jobs_.ConstructObject(name, color, this, job_function, num_dependencies);
        if (index != JPH::FixedSizeFreeList<Job>::cInvalidObjectIndex) {
            break;
        }
        AETHER_LOG_ERROR("Physics", "Jolt job pool exhausted; consider raising max_jobs");
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    Job* job = &jobs_.Get(index);
    JobHandle handle(job);

    if (num_dependencies == 0) {
        QueueJob(job);
    }
    return handle;
}

void JoltJobSystemAdapter::QueueJob(Job* job) {
    job->AddRef();
    job_system_.Schedule(
        [](void* data) {
            auto* j = static_cast<Job*>(data);
            j->Execute();
            j->Release();
        },
        job, dummy_counter_);
}

void JoltJobSystemAdapter::QueueJobs(Job** jobs, JPH::uint num_jobs) {
    for (JPH::uint i = 0; i < num_jobs; ++i) {
        QueueJob(jobs[i]);
    }
}

void JoltJobSystemAdapter::FreeJob(Job* job) {
    jobs_.DestructObject(job);
}

} // namespace aether
