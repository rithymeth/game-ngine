#include "aether/gameplay/tag_container.h"

#include <algorithm>

namespace aether::gas {

namespace {

auto Find(std::vector<TagCount>& v, const std::string& tag) {
    return std::lower_bound(v.begin(), v.end(), tag, [](const TagCount& e, const std::string& t) { return e.tag < t; });
}

} // namespace

bool TagContainer::Add(const GameplayTag& tag, i32 count) {
    if (!tag.IsValid() || count < 1) return false;
    const auto it = Find(tags, tag.name);
    if (it != tags.end() && it->tag == tag.name) it->count += count;
    else tags.insert(it, TagCount{tag.name, count});
    return true;
}

bool TagContainer::Remove(const GameplayTag& tag, i32 count) {
    if (count < 1) return false;
    const auto it = Find(tags, tag.name);
    if (it == tags.end() || it->tag != tag.name) return false;
    it->count -= count;
    if (it->count <= 0) tags.erase(it);
    return true;
}

bool TagContainer::RemoveAll(const GameplayTag& tag) {
    const auto it = Find(tags, tag.name);
    if (it == tags.end() || it->tag != tag.name) return false;
    tags.erase(it);
    return true;
}

bool TagContainer::HasTag(const GameplayTag& tag) const {
    if (!tag.IsValid()) return false;
    return std::any_of(tags.begin(), tags.end(), [&](const TagCount& e) { return GameplayTag{e.tag}.Matches(tag); });
}

bool TagContainer::HasTagExact(const GameplayTag& tag) const { return Count(tag) > 0; }

i32 TagContainer::Count(const GameplayTag& tag) const {
    const auto it = std::lower_bound(tags.begin(), tags.end(), tag.name, [](const TagCount& e, const std::string& t) { return e.tag < t; });
    return it != tags.end() && it->tag == tag.name ? it->count : 0;
}

bool TagContainer::HasAny(std::span<const GameplayTag> queries) const {
    return std::any_of(queries.begin(), queries.end(), [&](const GameplayTag& q) { return HasTag(q); });
}

bool TagContainer::HasAll(std::span<const GameplayTag> queries) const {
    return std::all_of(queries.begin(), queries.end(), [&](const GameplayTag& q) { return HasTag(q); });
}

std::vector<GameplayTag> TagContainer::Tags() const {
    std::vector<GameplayTag> out;
    out.reserve(tags.size());
    for (const TagCount& e : tags) out.push_back(GameplayTag{e.tag});
    return out;
}

} // namespace aether::gas
