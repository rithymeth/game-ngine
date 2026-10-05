#pragma once

#include "aether/core/base.h"
#include "aether/reflection/reflection.h"

#include <string>

namespace aether::loc {

// A player-visible string (Phase 29, §29.1): a stable `key` the string tables
// are indexed by, and the `source` text it was written with (the fallback
// when no table has the key, and what translators see). A LocText with an
// empty key is a literal: it is shown as it is and never looked up.
struct LocText {
    std::string key;
    std::string source;

    static LocText Literal(std::string text) { return LocText{{}, std::move(text)}; }
    bool IsLiteral() const { return key.empty(); }
    bool IsEmpty() const { return key.empty() && source.empty(); }
    bool operator==(const LocText& other) const = default;
};

} // namespace aether::loc

AETHER_REFLECT(aether::loc::LocText, 1,
    AETHER_FIELD(key, Field_EditAnywhere, {.tooltip = "The string table key"}),
    AETHER_FIELD(source, Field_EditAnywhere, {.tooltip = "The text as written: what shows when no table has the key"})
)
