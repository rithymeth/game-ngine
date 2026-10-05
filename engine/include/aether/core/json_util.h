#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>

namespace aether {

// The "$version" of a saved JSON document, or `fallback` if it is missing, not a whole number, or not a
// JSON object at all. nlohmann's `j.value("$version", 0)` throws on a string or an array where a number
// is expected, and nothing in a loader catches it: a malformed file would end the process. (Found by the
// fuzzers, Phase 47 step 1.)
inline int JsonVersion(const nlohmann::json& j, int fallback = 0) {
    if (!j.is_object()) return fallback;
    const auto it = j.find("$version");
    if (it == j.end() || !it->is_number_integer()) return fallback;
    const std::int64_t v = it->get<std::int64_t>();
    if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) return fallback;
    return static_cast<int>(v);
}

} // namespace aether
