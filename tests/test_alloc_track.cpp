#include "test_framework.h"

#include "aether/core/alloc_track.h"

#include <thread>

// The allocation counter's bookkeeping (Phase 38 step 7). The real operator new hook lives in tools/alloc_audit;
// here the counts come from calling the hook function directly.

using namespace aether;

AETHER_TEST(AllocTrack_CountsOnlyInsideAScopeAndOnlyOnItsThread) {
    alloc_track::OnAllocation(10);  // no scope open: ignored
    AllocScope scope;
    AETHER_CHECK(scope.Count() == 0 && scope.Bytes() == 0);
    alloc_track::OnAllocation(100);
    alloc_track::OnAllocation(28);
    AETHER_CHECK(scope.Count() == 2 && scope.Bytes() == 128);

    std::thread other([] {
        alloc_track::OnAllocation(1000);  // another thread, no scope there
        AllocScope inner;
        alloc_track::OnAllocation(5);
        AETHER_CHECK(inner.Count() == 1 && inner.Bytes() == 5);
    });
    other.join();
    AETHER_CHECK(scope.Count() == 2 && scope.Bytes() == 128);  // the other thread did not touch ours
}

AETHER_TEST(AllocTrack_ScopesNestAndRestart) {
    AllocScope outer;
    alloc_track::OnAllocation(1);
    {
        AllocScope inner;
        alloc_track::OnAllocation(2);
        alloc_track::OnAllocation(3);
        AETHER_CHECK(inner.Count() == 2 && inner.Bytes() == 5);
        AETHER_CHECK(outer.Count() == 3 && outer.Bytes() == 6);  // the outer one sees everything
    }
    alloc_track::OnAllocation(4);
    AETHER_CHECK(outer.Count() == 4 && outer.Bytes() == 10);
    AllocScope later;  // a new scope starts from zero
    AETHER_CHECK(later.Count() == 0);
}
