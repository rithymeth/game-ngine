#include "aether/job/job_system.h"
#include "aether/job/work_stealing_queue.h"
#include "test_framework.h"

#include <atomic>
#include <numeric>
#include <vector>

using namespace aether;

AETHER_TEST(WorkStealingQueue_OwnerPushPop) {
    WorkStealingQueue<int*, 16> queue;
    int a = 1, b = 2, c = 3;

    queue.Push(&a);
    queue.Push(&b);
    queue.Push(&c);

    int* out = nullptr;
    AETHER_CHECK(queue.Pop(out) && out == &c); // LIFO
    AETHER_CHECK(queue.Pop(out) && out == &b);
    AETHER_CHECK(queue.Pop(out) && out == &a);
    AETHER_CHECK(!queue.Pop(out));
}

AETHER_TEST(WorkStealingQueue_StealTakesFromOppositeEnd) {
    WorkStealingQueue<int*, 16> queue;
    int a = 1, b = 2, c = 3;
    queue.Push(&a);
    queue.Push(&b);
    queue.Push(&c);

    int* stolen = nullptr;
    AETHER_CHECK(queue.Steal(stolen) && stolen == &a); // FIFO from the top

    int* popped = nullptr;
    AETHER_CHECK(queue.Pop(popped) && popped == &c); // owner still LIFO from the bottom
}

AETHER_TEST(JobSystem_ScheduleAndWaitRunsAllJobs) {
    JobSystem jobs(3);
    std::atomic<i32> sum{0};
    JobCounter counter{0};

    struct Ctx { std::atomic<i32>* sum; };
    constexpr int kJobCount = 200;
    std::vector<Ctx> contexts(kJobCount, Ctx{&sum});

    for (int i = 0; i < kJobCount; ++i) {
        jobs.Schedule(
            [](void* data) {
                auto* ctx = static_cast<Ctx*>(data);
                ctx->sum->fetch_add(1, std::memory_order_relaxed);
            },
            &contexts[i], counter);
    }

    jobs.Wait(counter);
    AETHER_CHECK(sum.load() == kJobCount);
}

AETHER_TEST(JobSystem_ScheduleBatchSharesOneCounter) {
    JobSystem jobs(2);
    std::atomic<i32> sum{0};
    JobCounter counter{0};

    struct Ctx { std::atomic<i32>* sum; i32 value; };
    constexpr int kJobCount = 64;
    std::vector<Ctx> contexts;
    contexts.reserve(kJobCount);
    for (int i = 0; i < kJobCount; ++i) {
        contexts.push_back(Ctx{&sum, i + 1});
    }

    std::vector<JobDecl> decls;
    decls.reserve(kJobCount);
    for (auto& ctx : contexts) {
        decls.push_back(JobDecl{
            [](void* data) {
                auto* c = static_cast<Ctx*>(data);
                c->sum->fetch_add(c->value, std::memory_order_relaxed);
            },
            &ctx});
    }

    jobs.ScheduleBatch(decls.data(), static_cast<u32>(decls.size()), counter);
    jobs.Wait(counter);

    i32 expected = kJobCount * (kJobCount + 1) / 2;
    AETHER_CHECK(sum.load() == expected);
}

AETHER_TEST(JobSystem_NestedForkJoin) {
    JobSystem jobs(3);
    std::atomic<i32> leaf_count{0};

    struct LeafCtx { std::atomic<i32>* count; };
    struct ParentCtx {
        JobSystem* jobs;
        std::atomic<i32>* count;
        LeafCtx leaves[4];
    };

    constexpr int kParentCount = 10;
    std::vector<ParentCtx> parents(kParentCount);
    for (auto& p : parents) {
        p.jobs = &jobs;
        p.count = &leaf_count;
        for (auto& leaf : p.leaves) {
            leaf.count = &leaf_count;
        }
    }

    JobCounter parent_counter{0};
    for (auto& p : parents) {
        jobs.Schedule(
            [](void* data) {
                auto* parent = static_cast<ParentCtx*>(data);
                JobCounter child_counter{0};
                JobDecl children[4];
                for (int i = 0; i < 4; ++i) {
                    children[i] = JobDecl{
                        [](void* leaf_data) {
                            static_cast<LeafCtx*>(leaf_data)->count->fetch_add(1, std::memory_order_relaxed);
                        },
                        &parent->leaves[i]};
                }
                parent->jobs->ScheduleBatch(children, 4, child_counter);
                parent->jobs->Wait(child_counter); // nested wait from inside a running job
            },
            &p, parent_counter);
    }

    jobs.Wait(parent_counter);
    AETHER_CHECK(leaf_count.load() == kParentCount * 4);
}

AETHER_TEST(WorkStealingQueue_PushReportsFull) {
    WorkStealingQueue<int*, 4> queue;
    int values[5] = {};
    for (int i = 0; i < 4; ++i) {
        AETHER_CHECK(queue.Push(&values[i]));
    }
    AETHER_CHECK(!queue.Push(&values[4])); // full, not silently dropped

    int* out = nullptr;
    AETHER_CHECK(queue.Pop(out) && out == &values[3]);
    AETHER_CHECK(queue.Push(&values[4])); // room again after a pop
}

AETHER_TEST(JobSystem_ScheduleBatchLargerThanTheDeque) {
    JobSystem jobs(3);
    std::atomic<i32> count{0};

    // Well past kQueueCapacity (4096): the excess must spill to overflow and
    // still run exactly once, whether the owner or a thief picks it up.
    constexpr u32 kJobCount = 20000;
    std::vector<JobDecl> decls(kJobCount);
    for (JobDecl& decl : decls) {
        decl = JobDecl{
            [](void* data) { static_cast<std::atomic<i32>*>(data)->fetch_add(1, std::memory_order_relaxed); },
            &count};
    }

    JobCounter counter{0};
    jobs.ScheduleBatch(decls.data(), kJobCount, counter);
    jobs.Wait(counter);

    AETHER_CHECK(count.load() == static_cast<i32>(kJobCount));
}

AETHER_TEST(JobSystem_ScheduleSpillsWhenTheQueueIsFull) {
    JobSystem jobs(2);
    std::atomic<i32> count{0};
    constexpr u32 kJobCount = 6000;

    JobCounter counter{0};
    for (u32 i = 0; i < kJobCount; ++i) {
        jobs.Schedule(
            [](void* data) { static_cast<std::atomic<i32>*>(data)->fetch_add(1, std::memory_order_relaxed); },
            &count, counter);
    }
    jobs.Wait(counter);

    AETHER_CHECK(count.load() == static_cast<i32>(kJobCount));
}

AETHER_TEST(JobSystem_ReusesJobStorageAcrossBatches) {
    JobSystem jobs(2);
    std::atomic<i32> count{0};

    // Each round starts with nothing in flight, so the previous round's job
    // storage is reclaimed and reused. A stale pointer kept by a worker would
    // show up as a wrong count or a crash here.
    constexpr u32 kRounds = 20;
    constexpr u32 kBatch = 3000;
    for (u32 round = 0; round < kRounds; ++round) {
        JobCounter counter{0};
        std::vector<JobDecl> decls(kBatch);
        for (JobDecl& decl : decls) {
            decl = JobDecl{
                [](void* data) { static_cast<std::atomic<i32>*>(data)->fetch_add(1, std::memory_order_relaxed); },
                &count};
        }
        jobs.ScheduleBatch(decls.data(), kBatch, counter);
        jobs.Wait(counter);
    }

    AETHER_CHECK(count.load() == static_cast<i32>(kRounds * kBatch));
}

AETHER_TEST(JobSystem_NestedBatchSpillsAndCompletes) {
    JobSystem jobs(3);
    std::atomic<i32> leaves{0};

    // A parent running on a worker schedules a batch larger than a deque and
    // waits for it. This covers overflow reached from inside a job, where the
    // scheduling context is the worker's, and a nested Wait draining it.
    constexpr u32 kParents = 16;
    constexpr u32 kLeaves = 1200;
    struct Parent {
        JobSystem* jobs;
        std::atomic<i32>* leaves;
    };
    struct Leaf {
        std::atomic<i32>* leaves;
    };

    std::vector<Parent> parents(kParents, Parent{&jobs, &leaves});
    JobCounter parent_counter{0};
    for (Parent& parent : parents) {
        jobs.Schedule(
            [](void* data) {
                auto* p = static_cast<Parent*>(data);
                std::vector<Leaf> leaf_ctx(kLeaves, Leaf{p->leaves});
                std::vector<JobDecl> decls(kLeaves);
                for (u32 i = 0; i < kLeaves; ++i) {
                    decls[i] = JobDecl{
                        [](void* leaf_data) {
                            static_cast<Leaf*>(leaf_data)->leaves->fetch_add(1, std::memory_order_relaxed);
                        },
                        &leaf_ctx[i]};
                }
                JobCounter child_counter{0};
                p->jobs->ScheduleBatch(decls.data(), kLeaves, child_counter);
                p->jobs->Wait(child_counter);
            },
            &parent, parent_counter);
    }
    jobs.Wait(parent_counter);

    AETHER_CHECK(leaves.load() == static_cast<i32>(kParents * kLeaves));
}
