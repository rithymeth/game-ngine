#pragma once

#include "aether/core/base.h"

#include <functional>

namespace aether {

// A lightweight handle, not a pointer: `index` selects a slot in the
// EntityManager's dense record table, `generation` is bumped every time
// that slot is recycled so a stale handle to a destroyed entity is
// detectable instead of silently aliasing whatever now lives in that slot.
struct Entity {
    static constexpr u32 kInvalidIndex = static_cast<u32>(-1);

    u32 index = kInvalidIndex;
    u32 generation = 0;

    bool IsNull() const { return index == kInvalidIndex; }
    bool operator==(const Entity& o) const { return index == o.index && generation == o.generation; }
    bool operator!=(const Entity& o) const { return !(*this == o); }
};

inline constexpr Entity kNullEntity{};

} // namespace aether

template <>
struct std::hash<aether::Entity> {
    size_t operator()(const aether::Entity& e) const noexcept {
        return (static_cast<size_t>(e.index) << 32) ^ e.generation;
    }
};
