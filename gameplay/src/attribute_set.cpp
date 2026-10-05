#include "aether/gameplay/attribute_set.h"

#include <algorithm>
#include <cmath>

namespace aether::gas {

Attribute* AttributeSet::Find(const std::string& name) {
    const auto it = std::lower_bound(attributes.begin(), attributes.end(), name, [](const Attribute& a, const std::string& n) { return a.name < n; });
    return it != attributes.end() && it->name == name ? &*it : nullptr;
}

const Attribute* AttributeSet::Find(const std::string& name) const { return const_cast<AttributeSet*>(this)->Find(name); }

f32 AttributeSet::Get(const std::string& name, f32 fallback) const {
    const Attribute* a = Find(name);
    return a ? a->current : fallback;
}

f32 AttributeSet::Clamp(const Attribute& a, f32 value) {
    const f32 lo = std::min(a.min, a.max), hi = std::max(a.min, a.max);
    return std::clamp(value, lo, hi);
}

bool AttributeSet::Define(const std::string& name, f32 base, f32 min, f32 max) {
    if (name.empty() || std::isnan(base) || std::isnan(min) || std::isnan(max)) return false;
    Attribute* a = Find(name);
    if (a == nullptr) {
        const auto it = std::lower_bound(attributes.begin(), attributes.end(), name, [](const Attribute& e, const std::string& n) { return e.name < n; });
        a = &*attributes.insert(it, Attribute{});
        a->name = name;
    }
    a->min = std::min(min, max); // an upside-down range is the range the other way round
    a->max = std::max(min, max);
    a->base = Clamp(*a, base);
    RecomputeCurrent(*a);
    return true;
}

bool AttributeSet::SetBase(const std::string& name, f32 base) {
    Attribute* a = Find(name);
    if (a == nullptr || std::isnan(base)) return false;
    a->base = Clamp(*a, base);
    RecomputeCurrent(*a);
    return true;
}

bool AttributeSet::AddBase(const std::string& name, f32 delta) {
    Attribute* a = Find(name);
    if (a == nullptr || std::isnan(delta)) return false;
    return SetBase(name, a->base + delta);
}

void AttributeSet::Normalize() {
    std::sort(attributes.begin(), attributes.end(), [](const Attribute& x, const Attribute& y) { return x.name < y.name; });
    for (Attribute& a : attributes) {
        if (std::isnan(a.min)) a.min = -3.0e38f;
        if (std::isnan(a.max)) a.max = 3.0e38f;
        if (a.min > a.max) std::swap(a.min, a.max);
        a.base = std::isnan(a.base) ? 0.0f : Clamp(a, a.base);
        RecomputeCurrent(a);
    }
}

} // namespace aether::gas
