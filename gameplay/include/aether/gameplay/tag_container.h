#pragma once

#include "aether/gameplay/gameplay_tag.h"

#include <span>
#include <vector>

namespace aether::gas {

// One explicit tag and how many times it was added (§30.1).
struct TagCount {
    std::string tag;
    i32 count = 1;
};

// The tags something currently has (an entity's states, the tags an effect
// grants), each with a reference count: two effects that both grant
// "State.Stunned" keep the entity stunned until both are removed. Kept sorted,
// so it saves and compares deterministically. A query for a tag is
// hierarchical: having "Damage.Fire.Burning" satisfies a check for "Damage.Fire".
// It is a component (an entity's tags).
struct TagContainer {
    std::vector<TagCount> tags; // sorted by tag, counts > 0

    // Adds `count` (>= 1) of a valid tag; false for an invalid tag or count.
    bool Add(const GameplayTag& tag, i32 count = 1);
    // Takes away `count` of an explicit tag (it goes when the count reaches 0).
    // False if it wasn't there; removing more than it has removes it.
    bool Remove(const GameplayTag& tag, i32 count = 1);
    // Drops the tag whatever its count; false if it wasn't there.
    bool RemoveAll(const GameplayTag& tag);
    void Clear() { tags.clear(); }
    bool Empty() const { return tags.empty(); }

    // Hierarchical: some explicit tag is `tag` or under it.
    bool HasTag(const GameplayTag& tag) const;
    // Only the exact tag, not those under it.
    bool HasTagExact(const GameplayTag& tag) const;
    // The explicit tag's count (0 if it isn't there).
    i32 Count(const GameplayTag& tag) const;

    // Any / all / none of `queries` are satisfied (hierarchically). An empty
    // list: HasAny is false, HasAll and HasNone are true.
    bool HasAny(std::span<const GameplayTag> queries) const;
    bool HasAll(std::span<const GameplayTag> queries) const;
    bool HasNone(std::span<const GameplayTag> queries) const { return !HasAny(queries); }

    // The explicit tags (without counts), sorted.
    std::vector<GameplayTag> Tags() const;
    // Number of explicit tags.
    usize Size() const { return tags.size(); }
};

} // namespace aether::gas

AETHER_REFLECT(aether::gas::TagCount, 1, AETHER_FIELD(tag, Field_EditAnywhere), AETHER_FIELD(count, Field_EditAnywhere))
AETHER_REFLECT(aether::gas::TagContainer, 1, AETHER_FIELD(tags, Field_EditAnywhere, {.tooltip = "The tags, each with how many times it was added"}))
