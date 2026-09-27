#pragma once

#include "aether/assets/asset_guid.h"

#include <string>

namespace aether::assets {

// Asset type tags: which importer produces the asset an AssetRef points at.
struct ModelAsset {
    static constexpr const char* kImporter = "Model";
};
struct TextureAsset {
    static constexpr const char* kImporter = "Texture";
};
struct SceneAsset {
    static constexpr const char* kImporter = "Scene";
};
struct PrefabAsset {
    static constexpr const char* kImporter = "Prefab";
};
struct SoundAsset {
    static constexpr const char* kImporter = "Sound";
};
struct BlueprintAsset {
    static constexpr const char* kImporter = "Blueprint";
};
struct ScriptAsset {
    static constexpr const char* kImporter = "Script";
};
// Sub-assets split out of a model (ModelImporter).
struct MeshAsset {
    static constexpr const char* kImporter = "Mesh";
};
struct MaterialAsset {
    static constexpr const char* kImporter = "Material";
};
struct AnimationAsset {
    static constexpr const char* kImporter = "Animation";
};

// A reference to an asset by GUID (Phase 8, docs/design/PHASE_SPECS.md §8.1),
// typed by what kind of asset it must be. Saved as the GUID string, so it
// survives the asset being renamed or moved. A null GUID means "none".
namespace detail {
// Shared by every AssetRef<T> (all are a single AssetGuid, at offset 0):
// saved as the GUID string, "" for none.
nlohmann::json AssetRefToJson(const void* object);
bool AssetRefFromJson(const nlohmann::json& data, void* object);
} // namespace detail

template <typename AssetType>
struct AssetRef {
    AssetGuid guid;

    bool IsSet() const { return !guid.IsNull(); }
    bool operator==(const AssetRef& o) const { return guid == o.guid; }
    bool operator!=(const AssetRef& o) const { return guid != o.guid; }
};
static_assert(sizeof(AssetRef<ModelAsset>) == sizeof(AssetGuid));

} // namespace aether::assets

// Declared name "AssetRef<Model>" etc.; JSON form is the GUID string ("" for
// none); TypeInfo::asset_type names the importer for the editor's picker.
template <typename AssetType>
struct aether::reflect::Reflector<aether::assets::AssetRef<AssetType>> {
    static const ::aether::reflect::TypeInfo& Get() {
        static const std::string declared_name = std::string("AssetRef<") + AssetType::kImporter + ">";
        static const ::aether::reflect::TypeInfo info = [] {
            using namespace ::aether::reflect;
            using Self = aether::assets::AssetRef<AssetType>;
            TypeInfo type_info = detail::MakeTypeInfo<Self>("", TypeKind::Struct, 1);
            type_info.name = declared_name.c_str();
            type_info.id = HashTypeName(type_info.name);
            type_info.asset_type = AssetType::kImporter;
            detail::AddMembers(type_info, {AETHER_FIELD(guid)});
            return type_info;
        }();
        static const bool registered = [] {
            ::aether::reflect::TypeRegistry::Register(info);
            ::aether::reflect::RegisterJsonConverter(info, &aether::assets::detail::AssetRefToJson,
                                                     &aether::assets::detail::AssetRefFromJson);
            return true;
        }();
        (void)registered;
        return info;
    }
};
