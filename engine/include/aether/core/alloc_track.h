#pragma once

// @stability: experimental
//
// Allocation tracking (Phase 38 step 7, docs/design/UPGRADE_PLAN.md): counts the heap allocations a block of code
// makes, so a hot path can be shown to allocate nothing once it is warm.
//
// The engine does not replace global operator new (that is a program-wide decision, and it fights sanitizers).
// A program that wants counts defines operator new/delete itself and calls alloc_track::OnAllocation from the new
// one -- tools/alloc_audit does -- and then wraps the code in an AllocScope:
//
//     AllocScope scope;
//     RunOneFrame();
//     if (scope.Count() != 0) ...   // how many allocations this thread made, and their bytes
//
// Only the thread that made the scope is counted (a scope on the main thread does not count a worker's
// allocations, which is what a per-thread audit wants). Scopes nest: an inner scope's allocations also count in
// the outer one.

#include "aether/core/base.h"

namespace aether {

namespace alloc_track {
// Called by a program's operator new for every allocation of `bytes`. Cheap and allocation-free; a no-op on a
// thread with no AllocScope open.
void OnAllocation(usize bytes);
} // namespace alloc_track

class AllocScope {
public:
    AllocScope();
    ~AllocScope();
    AllocScope(const AllocScope&) = delete;
    AllocScope& operator=(const AllocScope&) = delete;

    u64 Count() const;  // allocations on this thread since the scope opened
    u64 Bytes() const;  // and their total size

private:
    u64 start_count_ = 0;
    u64 start_bytes_ = 0;
};

} // namespace aether
