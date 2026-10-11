#include "aether/core/alloc_track.h"

namespace aether {

namespace {
// Plain thread-locals: no allocation, no locking, nothing shared between threads.
thread_local u32 t_open_scopes = 0;
thread_local u64 t_count = 0;
thread_local u64 t_bytes = 0;
} // namespace

void alloc_track::OnAllocation(usize bytes) {
    if (t_open_scopes == 0) return;
    ++t_count;
    t_bytes += bytes;
}

AllocScope::AllocScope() : start_count_(t_count), start_bytes_(t_bytes) { ++t_open_scopes; }

AllocScope::~AllocScope() { --t_open_scopes; }

u64 AllocScope::Count() const { return t_count - start_count_; }

u64 AllocScope::Bytes() const { return t_bytes - start_bytes_; }

} // namespace aether
