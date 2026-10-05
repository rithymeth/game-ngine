#include "aether/gameplay/gameplay_tag.h"

namespace aether::gas {

bool GameplayTag::ValidName(std::string_view s) {
    if (s.empty() || s.size() > 256) return false;
    bool segment_has_chars = false;
    for (const char c : s) {
        if (c == '.') {
            if (!segment_has_chars) return false; // empty segment: a leading dot or "a..b"
            segment_has_chars = false;
            continue;
        }
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
        segment_has_chars = true;
    }
    return segment_has_chars; // no trailing dot
}

GameplayTag GameplayTag::Parent() const {
    const usize dot = name.rfind('.');
    return dot == std::string::npos ? GameplayTag{} : GameplayTag{name.substr(0, dot)};
}

i32 GameplayTag::Depth() const {
    if (name.empty()) return 0;
    i32 depth = 1;
    for (const char c : name) depth += c == '.';
    return depth;
}

bool GameplayTag::Matches(const GameplayTag& other) const {
    if (!IsValid() || !other.IsValid()) return false;
    if (name == other.name) return true;
    // Under `other`: its name followed by a dot, so "State.Stunned" is under "State" but "StateX" and "State2" are not.
    return name.size() > other.name.size() && name.compare(0, other.name.size(), other.name) == 0 && name[other.name.size()] == '.';
}

} // namespace aether::gas
