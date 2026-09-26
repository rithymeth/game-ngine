#pragma once

#include "aether/core/base.h"
#include "aether/reflection/converters.h"
#include "aether/reflection/reflection.h"

#include <functional>
#include <string>
#include <string_view>

namespace aether::assets {

// Permanent identity of an asset (Phase 8, docs/design/PHASE_SPECS.md §8.1):
// a random version 4 UUID stored in the asset's .ameta sidecar. References
// between assets and from scenes use it rather than the file path, so
// renaming or moving files never breaks them. Same format as EntityGuid, but
// a distinct type so the two can't be mixed up.
struct AssetGuid {
    u64 hi = 0;
    u64 lo = 0;

    bool IsNull() const { return hi == 0 && lo == 0; }
    bool operator==(const AssetGuid& o) const { return hi == o.hi && lo == o.lo; }
    bool operator!=(const AssetGuid& o) const { return !(*this == o); }
    bool operator<(const AssetGuid& o) const { return hi != o.hi ? hi < o.hi : lo < o.lo; }
};

AssetGuid NewAssetGuid();
std::string ToString(const AssetGuid& guid);
bool ParseAssetGuid(std::string_view text, AssetGuid& out);

namespace detail {
nlohmann::json AssetGuidToJson(const void* object);
bool AssetGuidFromJson(const nlohmann::json& data, void* object);
} // namespace detail

} // namespace aether::assets

// Saved as its canonical string, like EntityGuid.
template <>
struct aether::reflect::Reflector<aether::assets::AssetGuid> {
    static const ::aether::reflect::TypeInfo& Get() {
        static const ::aether::reflect::TypeInfo info = [] {
            using namespace ::aether::reflect;
            using Self = aether::assets::AssetGuid;
            TypeInfo type_info = detail::MakeTypeInfo<Self>("AssetGuid", TypeKind::Struct, 1);
            detail::AddMembers(type_info, {AETHER_FIELD(hi), AETHER_FIELD(lo)});
            return type_info;
        }();
        static const bool registered = [] {
            ::aether::reflect::TypeRegistry::Register(info);
            ::aether::reflect::RegisterJsonConverter(info, &aether::assets::detail::AssetGuidToJson,
                                                     &aether::assets::detail::AssetGuidFromJson);
            return true;
        }();
        (void)registered;
        return info;
    }
    static inline const bool kStaticRegistration = (Get(), true);
};

template <>
struct std::hash<aether::assets::AssetGuid> {
    size_t operator()(const aether::assets::AssetGuid& g) const noexcept {
        return static_cast<size_t>(g.hi ^ (g.lo * 0x9e3779b97f4a7c15ull));
    }
};
