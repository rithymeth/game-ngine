#include "world/content_drop.h"

#include "content/content_drag.h"
#include "core/snapping.h"

#include "aether/assets/asset_database.h"
#include "aether/sprite2d/atlas.h"
#include "aether/sprite2d/components.h"
#include "aether/sprite2d/tilemap.h"

#include <algorithm>
#include <cmath>

namespace aether::editor {

namespace {

bool IsTilemap(const std::string& path) { return HasAssetExtension(path, ".atilemap"); }
bool IsSpriteAtlas(const std::string& path) { return HasAssetExtension(path, ".aatlas"); }

Vec3 Drop2DPoint(const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size,
                      const ImVec2& mouse, f32 snap_step) {
    Vec3 point(camera.target.x, camera.target.y, 0.0f);
    Vec3 ray_start, ray_direction;
    if (size.x >= 1.0f && size.y >= 1.0f &&
        MouseRay(camera.ViewProjection(size.x / size.y), origin, size, mouse, ray_start, ray_direction) &&
        std::fabs(ray_direction.z) > 1e-5f) {
        const f32 distance = -ray_start.z / ray_direction.z;
        if (distance >= 0.0f) point = ray_start + ray_direction * distance;
    }
    point.z = 0.0f; // 2D sprites and maps are authored on the XY plane.
    return snap_step > 0.0f ? SnapTranslation(point, snap_step) : point;
}

template <class Component>
std::string Create2DEntity(SceneDocument& document, const std::string& path, const Vec3& at,
                           Component renderer, bool* placed, Entity parent) {
    const std::string name = std::filesystem::path(path).stem().string();
    auto& world = document.GetWorld();
    const EntityGuid guid = NewEntityGuid();
    const Entity entity = world.CreateEntity(EntityName{name}, Transform{at, Quaternion::Identity()}, IdComponent{guid}, renderer);
    if (!parent.IsNull() && world.IsAlive(parent)) {
        if (const auto* id = world.GetComponent<IdComponent>(parent)) world.AddComponent(entity, Parent{id->guid});
    }
    document.Guids().Add(guid, entity);
    document.Commands().Record(CreateEntityCommand::FromExisting(document.Context(), entity, "Place " + name));
    document.SetSelection({entity});
    if (placed) *placed = true;
    return "Placed " + path;
}

std::string PlaceTilemap(SceneDocument& document, const std::filesystem::path& content_root,
                         const std::string& path, const Vec3& at, bool* placed, Entity parent) {
    if (content_root.empty()) return "Could not place " + path + ": no project Content folder.";
    assets::AssetDatabase database(content_root);
    database.Scan();
    const auto* record = database.FindByPath(path);
    if (!record || record->missing || record->importer != "Tilemap" || record->guid.IsNull())
        return "Could not place " + path + ": tilemap not found.";
    sprite2d::TilemapData map;
    std::string error;
    if (!sprite2d::LoadTilemap(database.SourcePath(record->guid), map, &error))
        return "Could not place " + path + ": " + error;
    const auto* tileset = database.Find(map.tileset);
    if (!tileset || tileset->missing || tileset->importer != "Tileset")
        return "Could not place " + path + ": its tileset is missing.";
    sprite2d::Tileset tileset_data;
    if (!sprite2d::LoadTileset(database.SourcePath(tileset->guid), tileset_data, &error))
        return "Could not place " + path + ": " + tileset->path + ": " + error;
    sprite2d::TilemapRenderer renderer;
    renderer.tilemap.guid = record->guid;
    return Create2DEntity(document, path, at, renderer, placed, parent);
}

std::string PlaceSpriteAtlas(SceneDocument& document, const std::filesystem::path& content_root,
                             const std::string& path, const Vec3& at, bool* placed, Entity parent) {
    if (content_root.empty()) return "Could not place " + path + ": no project Content folder.";
    assets::AssetDatabase database(content_root);
    database.Scan();
    const auto* record = database.FindByPath(path);
    if (!record || record->missing || record->importer != "SpriteAtlas" || record->guid.IsNull())
        return "Could not place " + path + ": sprite atlas not found.";
    sprite2d::SpriteAtlas atlas;
    std::string error;
    if (!sprite2d::LoadAtlas(database.SourcePath(record->guid), atlas, &error))
        return "Could not place " + path + ": " + error;
    const auto* texture = database.Find(atlas.texture);
    if (!texture || texture->missing || texture->importer != "Texture")
        return "Could not place " + path + ": its texture is missing.";
    sprite2d::Sprite sprite;
    const auto frame = std::find_if(atlas.frames.begin(), atlas.frames.end(), [&](const auto& f) {
        return f.name.size() < sizeof(sprite.frame) && f.name.find('\0') == std::string::npos;
    });
    if (frame == atlas.frames.end())
        return "Could not place " + path + ": no frame name fits a Sprite component.";
    sprite.atlas.guid = record->guid;
    sprite2d::SetSpriteFrame(sprite, frame->name);
    return Create2DEntity(document, path, at, sprite, placed, parent);
}

} // namespace

Vec3 DropPoint(const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step) {
    Vec3 ground;
    if (size.x < 1.0f || size.y < 1.0f || !GroundPoint(camera.ViewProjection(size.x / size.y), origin, size, mouse, ground)) ground = camera.target;
    if (snap_step > 0.0f) ground = SnapTranslation(ground, snap_step);
    return ground;
}

std::string PlaceContentAsset(SceneDocument& document, const std::filesystem::path& content_root, const std::string& content_path, const Vec3& at, bool* placed, Entity parent) {
    if (placed) *placed = false;
    if (IsTilemap(content_path)) return PlaceTilemap(document, content_root, content_path, at, placed, parent);
    if (IsSpriteAtlas(content_path)) return PlaceSpriteAtlas(document, content_root, content_path, at, placed, parent);
    if (IsPlaceablePrefab(content_path)) {
        if (content_root.empty()) return "Could not place " + content_path;
        std::string problem;
        const Entity entity = PlacePrefabAsset(document, content_root, content_path, at, &problem, parent);
        if (entity == kNullEntity) return problem.empty() ? "Could not place " + content_path : problem;
        if (placed) *placed = true;
        return "Placed " + content_path;
    }
    if (IsPlaceableModel(content_path)) {
        const Entity entity = PlaceModelAsset(document, content_path, at, parent);
        if (entity == kNullEntity) return "Could not place " + content_path;
        if (placed) *placed = true;
        return "Placed " + content_path;
    }
    return "Only models (.gltf, .glb), prefabs (.aprefab), tilemaps (.atilemap), and sprite atlases (.aatlas) can be placed in the scene.";
}

void ContentDrop::Accept(SceneDocument& document, const std::filesystem::path& content_root, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, f32 snap_step) {
    preview = false;
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kContentPathPayload, ImGuiDragDropFlags_AcceptBeforeDelivery)) {
        const std::string path(static_cast<const char*>(payload->Data));
        const bool asset_2d = IsTilemap(path) || IsSpriteAtlas(path);
        const auto& mouse = ImGui::GetIO().MousePos;
        const Vec3 at = asset_2d ? Drop2DPoint(camera, origin, size, mouse, snap_step)
                                : DropPoint(camera, origin, size, mouse, snap_step);
        if (payload->IsDelivery()) {
            note = PlaceContentAsset(document, content_root, path, at);
        } else if (asset_2d || IsPlaceableModel(path) || IsPlaceablePrefab(path)) {
            preview = true; // still being dragged: a marker where it would land
            point = at;
        }
    }
    ImGui::EndDragDropTarget();
}

void ContentDrop::DrawPreview(ImDrawList* draw, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size) const {
    if (!preview || size.x < 1.0f || size.y < 1.0f) return;
    ImVec2 at;
    if (!ProjectToViewport(camera.ViewProjection(size.x / size.y), point, origin, size, at)) return;
    draw->AddCircle(at, 10.0f, IM_COL32(255, 200, 60, 255), 24, 2.0f);
    draw->AddCircleFilled(at, 3.0f, IM_COL32(255, 200, 60, 255));
}

} // namespace aether::editor
