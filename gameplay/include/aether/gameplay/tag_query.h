#pragma once

#include "aether/gameplay/tag_container.h"
#include "aether/reflection/serialize.h"

#include <string>
#include <vector>

namespace aether::gas {

// A condition on a TagContainer (§30.1): what an effect needs to apply, what an
// ability requires or is blocked by. A tree:
//   Any   some listed tag is present        All   every listed tag is present
//   None  no listed tag is present          And / Or   of the child queries
// Tags match hierarchically (see TagContainer::HasTag). An empty And is true, an
// empty Or false; an empty Any is false, an empty All or None true. Saved as JSON,
// e.g. {"op":"and","children":[{"op":"all","tags":["State.Alive"]},{"op":"none","tags":["State.Stunned"]}]}.
struct TagQuery {
    enum class Op : u8 { Any, All, None, And, Or };
    Op op = Op::All;
    std::vector<GameplayTag> tags;   // Any, All, None
    std::vector<TagQuery> children;  // And, Or

    static TagQuery Any(std::vector<GameplayTag> t) { return {Op::Any, std::move(t), {}}; }
    static TagQuery All(std::vector<GameplayTag> t) { return {Op::All, std::move(t), {}}; }
    static TagQuery None(std::vector<GameplayTag> t) { return {Op::None, std::move(t), {}}; }
    static TagQuery And(std::vector<TagQuery> c) { return {Op::And, {}, std::move(c)}; }
    static TagQuery Or(std::vector<TagQuery> c) { return {Op::Or, {}, std::move(c)}; }

    bool Matches(const TagContainer& container) const;
    bool IsEmpty() const { return tags.empty() && children.empty(); }

    reflect::Json ToJson() const;
    // False (with `error`) if the JSON isn't a query: an unknown op, a bad tag name, a wrong shape.
    static bool FromJson(const reflect::Json& j, TagQuery& out, std::string* error = nullptr);
    bool operator==(const TagQuery& other) const = default;
};

} // namespace aether::gas
