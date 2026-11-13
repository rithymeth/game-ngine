#include "core/scene_document.h"

#include "aether/ecs/archetype.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"
#include "core/commands.h"

#include <algorithm>
#include <string>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace aether::editor {
namespace {

std::string HumanizeIdentifier(std::string value) {
    std::string result;
    result.reserve(value.size() + 8);
    bool capitalize = true;
    char previous = '\0';
    for (char character : value) {
        if (character == '_' || character == '-') {
            if (!result.empty() && result.back() != ' ') result.push_back(' ');
            capitalize = true;
            previous = character;
            continue;
        }
        const bool camel_boundary = !result.empty() && previous != '\0' &&
            std::islower(static_cast<unsigned char>(previous)) &&
            std::isupper(static_cast<unsigned char>(character));
        if (camel_boundary && result.back() != ' ') result.push_back(' ');
        result.push_back(capitalize ? static_cast<char>(std::toupper(static_cast<unsigned char>(character))) : character);
        capitalize = false;
        previous = character;
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

std::string SuggestedEntityName(const World& world, Entity entity) {
    if (const auto* tags = world.GetComponent<Tags>(entity)) {
        for (auto item = tags->names.rbegin(); item != tags->names.rend(); ++item) {
            if (item->empty() || *item == "Greybox") continue;
            return HumanizeIdentifier(*item);
        }
    }
    if (const auto* renderer = world.GetComponent<ModelRenderer>(entity)) {
        std::string stem = std::filesystem::path(renderer->asset_path).stem().string();
        constexpr char kGreyboxPrefix[] = "aether_greybox_";
        if (stem.starts_with(kGreyboxPrefix)) stem.erase(0, sizeof(kGreyboxPrefix) - 1);
        if (!stem.empty()) return HumanizeIdentifier(stem);
    }
    if (world.HasComponent<Camera>(entity)) return "Camera";
    return "Entity " + std::to_string(entity.index);
}

} // namespace

SceneDocument::SceneDocument() : context_(world_, guids_) {
    commands_.SetAttribution("user");
}

void SceneDocument::Reset() {
    if (!play_session_.IsEditing()) play_session_.Stop(context_, commands_);
    world_ = World{};
    guids_.Clear();
    selection_.clear(); // Phase 43.3: the selection belongs to the scene that was open
    commands_.Clear();
    ++local_revision_; // a new or loaded scene is a change to whoever is looking at the old one
    file_.clear();
    recovered_unsaved_ = false;
    pending_position_before_.reset();
    pending_position_entity_ = {};
    pending_rotation_before_.reset();
    pending_rotation_entity_ = {};
    pending_scale_before_.reset();
    pending_scale_entity_ = {};
}

void SceneDocument::NewScene() {
    Reset();
    const auto add = [this](const char* name, Vec3 position, bool camera) {
        const EntityGuid guid = NewEntityGuid();
        const Entity entity = camera
            ? world_.CreateEntity(EntityName{name}, Transform{position, Quaternion::Identity()}, Camera{}, IdComponent{guid})
            : world_.CreateEntity(EntityName{name}, Transform{position, Quaternion::Identity()}, IdComponent{guid});
        guids_.Add(guid, entity);
    };
    add("Camera", Vec3{0.0f, 3.0f, -8.0f}, true);
    add("Directional Light", Vec3{2.0f, 4.0f, 0.0f}, false);
    add("Environment", Vec3{0.0f, 0.0f, 0.0f}, false);
    add("Player", Vec3{0.0f, 1.8f, -6.0f}, false);
    add("Ground", Vec3{0.0f, -0.5f, 0.0f}, false);
    commands_.MarkSaved();
}

bool SceneDocument::Load(const std::filesystem::path& file, std::string* error) {
    if (!play_session_.IsEditing()) {
        if (error) *error = "Stop Play mode before opening another scene.";
        return false;
    }
    const std::string path = file.string();
    World candidate;
    std::vector<std::string> unknown_components;
    const SceneLoadOptions editor_load_options{.reject_unknown_components = true, .unknown_components = &unknown_components};
    Vec3d loaded_origin;
    const bool loaded = file.extension() == ".ascene"
        ? LoadSceneJson(candidate, path, editor_load_options, &loaded_origin)
        : LoadScene(candidate, path, editor_load_options);
    if (!loaded) {
        if (error) {
            if (unknown_components.empty()) {
                *error = "The scene couldn't be loaded: " + path + " is damaged or not a scene this version reads. Check the Output panel for the reason.";
            } else {
                std::string names;
                for (const std::string& name : unknown_components) names += (names.empty() ? "" : ", ") + name;
                *error = "The scene was not opened: it has components this build does not know (" + names + "), and saving it would delete them from the file. Open it in a build that has them (a plugin that is not loaded, or a newer engine).";
            }
        }
        return false;
    }
    origin_ = loaded_origin;

    GuidIndex candidate_guids;
    EnsureAllGuids(candidate, candidate_guids);
    std::vector<Entity> entities;
    candidate.ForEachArchetype([&entities](const Archetype& archetype) {
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            const u32 count = archetype.ChunkEntityCount(chunk);
            const Entity* array = archetype.EntityArray(chunk);
            entities.insert(entities.end(), array, array + count);
        }
    });
    std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return a.index < b.index; });
    // How many entities hold each name, so one that is about to be renamed can let go of its own first.
    std::unordered_map<std::string, u32> name_counts;
    for (Entity entity : entities) {
        if (const auto* name = candidate.GetComponent<EntityName>(entity); name && !name->value.empty())
            ++name_counts[name->value];
    }
    for (Entity entity : entities) {
        auto* existing_name = candidate.GetComponent<EntityName>(entity);
        const std::string legacy_placeholder = "Entity " + std::to_string(entity.index);
        if (existing_name && !existing_name->value.empty() && existing_name->value != legacy_placeholder) continue;
        const std::string base = SuggestedEntityName(candidate, entity);
        // An entity that already has the placeholder name lets go of it before the name is chosen: otherwise its own name counts as taken and it is renamed
        // to "Entity 2 2" the second time the scene is opened (a placeholder saved by the first open), though nothing else has that name.
        if (existing_name && !existing_name->value.empty()) --name_counts[existing_name->value];
        std::string unique = base;
        for (u32 suffix = 2; name_counts[unique] > 0; ++suffix)
            unique = base + " " + std::to_string(suffix);
        if (existing_name) existing_name->value = unique;
        else candidate.AddComponent(entity, EntityName{unique});
        ++name_counts[unique];
    }
    world_ = std::move(candidate);
    guids_ = std::move(candidate_guids);
    selection_.clear(); // Handles belong to the old world, even if the new world reuses their indices.
    commands_.Clear();
    ++local_revision_; // Replacing a world must refresh viewers even when no edit commands were issued.
    pending_position_before_.reset();
    pending_position_entity_ = {};
    pending_rotation_before_.reset();
    pending_rotation_entity_ = {};
    pending_scale_before_.reset();
    pending_scale_entity_ = {};
    file_ = file;
    recovered_unsaved_ = false;
    commands_.MarkSaved();
    return true;
}

bool SceneDocument::Save(const std::filesystem::path& file, std::string* error) {
    if (!play_session_.IsEditing()) {
        if (error) *error = "Stop Play mode before saving the scene.";
        return false;
    }
    const std::filesystem::path target = file.empty() ? file_ : file;
    if (target.empty()) {
        if (error) *error = "Choose a path before saving this scene.";
        return false;
    }
    const std::string path = target.string();
    const bool saved = target.extension() == ".ascene" ? SaveSceneJson(world_, path, SceneSaveOptions{origin_})
                                                     : SaveScene(world_, path);
    if (!saved) {
        if (error) *error = "The scene couldn't be saved.";
        return false;
    }
    file_ = target;
    recovered_unsaved_ = false;
    commands_.MarkSaved();
    return true;
}

bool SceneDocument::SaveAutosave(const std::filesystem::path& file, std::string* error) const {
    if (!play_session_.IsEditing()) {
        if (error) *error = "Stop Play mode before autosaving the scene.";
        return false;
    }
    if (file.empty()) {
        if (error) *error = "Choose an autosave path.";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        if (error) *error = "The autosave directory couldn't be created: " + ec.message();
        return false;
    }
    if (!SaveSceneJson(world_, file.string(), SceneSaveOptions{origin_})) {
        if (error) *error = "The scene couldn't be autosaved.";
        return false;
    }
    return true;
}

bool SceneDocument::LoadAutosave(const std::filesystem::path& autosave, const std::filesystem::path& source,
                                 std::string* error) {
    if (!Load(autosave, error)) return false;
    file_ = source;
    recovered_unsaved_ = true;
    return true;
}

void SceneDocument::SetSelection(std::vector<Entity> entities) {
    selection_ = std::move(entities);
    // Deduplicate but keep the caller's order: the gizmo's pivot follows the first one.
    std::vector<Entity> unique;
    unique.reserve(selection_.size());
    for (const Entity entity : selection_) {
        if (std::find(unique.begin(), unique.end(), entity) == unique.end()) unique.push_back(entity);
    }
    selection_ = std::move(unique);
}

void SceneDocument::ToggleSelection(Entity entity) {
    const auto at = std::find(selection_.begin(), selection_.end(), entity);
    if (at == selection_.end()) {
        selection_.push_back(entity);
    } else {
        selection_.erase(at);
    }
}

bool SceneDocument::IsSelected(Entity entity) const {
    return std::find(selection_.begin(), selection_.end(), entity) != selection_.end();
}

void SceneDocument::ClearSelection() { selection_.clear(); }
bool SceneDocument::CopySelection() {
    clipboard_.clear();
    for (const Entity entity : selection_) {
        if (world_.IsAlive(entity)) clipboard_.push_back(EntitySnapshot::Capture(world_, entity));
    }
    return !clipboard_.empty();
}

usize SceneDocument::PasteClipboard(const Vec3& offset) {
    usize pasted = 0;
    for (const EntitySnapshot& snapshot : clipboard_) {
        Entity entity = snapshot.Restore(world_);
        if (entity.IsNull()) continue;
        RegenerateGuid(world_, entity, &guids_);
        if (auto* name = world_.GetComponent<EntityName>(entity)) name->value += " Copy";
        if (auto* transform = world_.GetComponent<Transform>(entity)) transform->position = transform->position + offset;
        commands_.Record(CreateEntityCommand::FromExisting(context_, entity, "Paste " + Name(entity)));
        ++pasted;
    }
    return pasted;
}
std::vector<Entity> SceneDocument::Entities() const {
    std::vector<Entity> entities;
    world_.ForEachArchetype([&entities](const Archetype& archetype) {
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            const u32 count = archetype.ChunkEntityCount(chunk);
            const Entity* array = archetype.EntityArray(chunk);
            entities.insert(entities.end(), array, array + count);
        }
    });
    std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return a.index < b.index; });
    return entities;
}

std::string SceneDocument::Name(Entity entity) const {
    if (!world_.IsAlive(entity)) return {};
    if (const auto* name = world_.GetComponent<EntityName>(entity)) return name->value;
    return "Entity " + std::to_string(entity.index);
}

bool SceneDocument::Rename(Entity entity, const std::string& name, bool committed) {
    if (!world_.IsAlive(entity) || !world_.HasComponent<EntityName>(entity)) return false;
    const auto* id = world_.GetComponent<IdComponent>(entity);
    auto* component = world_.GetComponent<EntityName>(entity);
    if (!id || !component) return false;
    const auto& type = reflect::Reflect<EntityName>();
    const auto field = std::find_if(type.fields.begin(), type.fields.end(),
                                    [](const reflect::FieldInfo& candidate) {
                                        return std::strcmp(candidate.name, "value") == 0;
                                    });
    if (field == type.fields.end()) return false;
    const reflect::Any old_value = field->Get(component);
    field->Set(component, reflect::Any(name));
    CommitFieldEdit(context_, commands_, id->guid, GetComponentId<EntityName>(), *field, component,
                    old_value, committed);
    return true;
}

Entity SceneDocument::CreateEntity(const std::string& name, SceneEntityKind kind,
                                   const std::string& model_asset_path, Entity parent) {
    const EntityGuid guid = NewEntityGuid();
    const Entity entity = world_.CreateEntity(EntityName{name}, Transform{}, IdComponent{guid});
    if (kind == SceneEntityKind::Camera) {
        world_.AddComponent(entity, Camera{});
    } else if (kind == SceneEntityKind::CineCamera) {
        world_.AddComponent(entity, Camera{});
        world_.AddComponent(entity, CineCamera{});
    } else if (kind == SceneEntityKind::Model) {
        ModelRenderer renderer;
        SetModelPath(renderer, model_asset_path);
        world_.AddComponent(entity, renderer);
    }
    if (!parent.IsNull() && world_.IsAlive(parent)) {
        if (const auto* parent_id = world_.GetComponent<IdComponent>(parent))
            world_.AddComponent(entity, Parent{parent_id->guid});
    }
    guids_.Add(guid, entity);
    commands_.Record(CreateEntityCommand::FromExisting(context_, entity, "Create " + name));
    return entity;
}

Entity SceneDocument::DuplicateEntity(Entity entity, const Vec3& offset) {
    if (!world_.IsAlive(entity)) return kNullEntity;
    Entity duplicate = EntitySnapshot::Capture(world_, entity).Restore(world_);
    RegenerateGuid(world_, duplicate, &guids_);
    if (auto* name = world_.GetComponent<EntityName>(duplicate)) name->value += " Copy";
    if (auto* transform = world_.GetComponent<Transform>(duplicate))
        transform->position = transform->position + offset;
    commands_.Record(CreateEntityCommand::FromExisting(context_, duplicate,
        "Duplicate " + Name(entity)));
    return duplicate;
}

Entity SceneDocument::ParentOf(Entity entity) const {
    if (!world_.IsAlive(entity)) return kNullEntity;
    return GetParent(world_, guids_, entity);
}

bool SceneDocument::ReparentEntity(Entity entity, Entity new_parent) {
    if (!world_.IsAlive(entity)) return false;
    if (!new_parent.IsNull() && !world_.IsAlive(new_parent)) return false;
    if (!new_parent.IsNull() && WouldCreateCycle(world_, guids_, entity, new_parent)) return false;
    const auto* entity_id = world_.GetComponent<IdComponent>(entity);
    if (!entity_id) return false;
    const Entity current_parent = GetParent(world_, guids_, entity);
    if (current_parent == new_parent) return false;
    EntityGuid parent_guid{};
    if (!new_parent.IsNull()) {
        const auto* parent_id = world_.GetComponent<IdComponent>(new_parent);
        if (!parent_id) return false;
        parent_guid = parent_id->guid;
    }
    commands_.Execute(context_, std::make_unique<ReparentCommand>(entity_id->guid, parent_guid));
    return true;
}

bool SceneDocument::DestroyEntity(Entity entity) {
    if (!world_.IsAlive(entity)) return false;
    const auto* id = world_.GetComponent<IdComponent>(entity);
    if (!id) return false;
    commands_.Execute(context_, std::make_unique<DestroyEntityCommand>(id->guid));
    return true;
}

bool SceneDocument::AddComponent(Entity entity, ComponentId component) {
    if (!world_.IsAlive(entity) || component >= RegisteredComponentCount() ||
        world_.HasComponentRaw(entity, component)) return false;
    const auto* id = world_.GetComponent<IdComponent>(entity);
    if (!id) return false;
    commands_.Execute(context_, std::make_unique<AddComponentCommand>(id->guid, component));
    return true;
}

bool SceneDocument::RemoveComponent(Entity entity, ComponentId component) {
    if (!world_.IsAlive(entity) || component >= RegisteredComponentCount() ||
        !world_.HasComponentRaw(entity, component)) return false;
    const auto* id = world_.GetComponent<IdComponent>(entity);
    if (!id) return false;
    commands_.Execute(context_, std::make_unique<RemoveComponentCommand>(id->guid, component));
    return true;
}

bool SceneDocument::SetComponentField(Entity entity, ComponentId component, const std::string& field_name,
                                      const reflect::Any& value, bool committed) {
    if (!world_.IsAlive(entity) || component >= RegisteredComponentCount() ||
        !world_.HasComponentRaw(entity, component)) return false;
    const auto* id = world_.GetComponent<IdComponent>(entity);
    const ComponentInfo& info = GetComponentInfo(component);
    void* data = world_.GetComponentRaw(entity, component);
    if (!id || !info.reflected || !data) return false;
    const auto field = std::find_if(info.reflected->fields.begin(), info.reflected->fields.end(),
        [&](const reflect::FieldInfo& candidate) { return field_name == candidate.name; });
    if (field == info.reflected->fields.end()) return false;
    const reflect::Any old_value = field->Get(data);
    if (!field->Set(data, value)) return false;
    // CommitFieldEdit restores the live value while recording the reversible change.
    CommitFieldEdit(context_, commands_, id->guid, component, *field, data, old_value, committed);
    return true;
}

bool SceneDocument::SetPosition(Entity entity, const Vec3& position, bool committed) {
    if (!world_.IsAlive(entity) || !world_.HasComponent<Transform>(entity)) return false;
    const auto* id = world_.GetComponent<IdComponent>(entity);
    auto* transform = world_.GetComponent<Transform>(entity);
    if (!id || !transform) return false;
    const auto& type = reflect::Reflect<Transform>();
    const auto field = std::find_if(type.fields.begin(), type.fields.end(),
                                    [](const reflect::FieldInfo& candidate) {
                                        return std::strcmp(candidate.name, "position") == 0;
                                    });
    if (field == type.fields.end()) return false;
    const EntityGuid guid = id->guid;
    if (!pending_position_before_ || pending_position_entity_ != guid) {
        pending_position_before_ = field->Get(transform);
        pending_position_entity_ = guid;
    }
    const reflect::Any old_value = *pending_position_before_;
    field->Set(transform, reflect::Any(position));
    CommitFieldEdit(context_, commands_, id->guid, GetComponentId<Transform>(), *field, transform,
                    old_value, committed);
    if (committed) {
        pending_position_before_.reset();
        pending_position_entity_ = {};
    }
    return true;
}

bool SceneDocument::SetRotation(Entity entity, const Quaternion& rotation, bool committed) {
    if (!world_.IsAlive(entity) || !world_.HasComponent<Transform>(entity)) return false;
    const auto* id = world_.GetComponent<IdComponent>(entity);
    auto* transform = world_.GetComponent<Transform>(entity);
    if (!id || !transform) return false;
    const auto& type = reflect::Reflect<Transform>();
    const auto field = std::find_if(type.fields.begin(), type.fields.end(),
                                    [](const reflect::FieldInfo& candidate) {
                                        return std::strcmp(candidate.name, "rotation") == 0;
                                    });
    if (field == type.fields.end()) return false;
    const EntityGuid guid = id->guid;
    if (!pending_rotation_before_ || pending_rotation_entity_ != guid) {
        pending_rotation_before_ = field->Get(transform);
        pending_rotation_entity_ = guid;
    }
    const reflect::Any old_value = *pending_rotation_before_;
    const Quaternion normalized = rotation.Normalized();
    field->Set(transform, reflect::Any(normalized));
    CommitFieldEdit(context_, commands_, id->guid, GetComponentId<Transform>(), *field, transform,
                    old_value, committed);
    if (committed) {
        pending_rotation_before_.reset();
        pending_rotation_entity_ = {};
    }
    return true;
}

bool SceneDocument::SetScale(Entity entity, const Vec3& scale, bool committed) {
    if (!world_.IsAlive(entity) || !world_.HasComponent<Transform>(entity)) return false;
    const auto* id = world_.GetComponent<IdComponent>(entity);
    auto* transform = world_.GetComponent<Transform>(entity);
    if (!id || !transform) return false;
    if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z)) return false;
    const auto prevent_zero_scale = [](f32 value) {
        if (std::abs(value) >= 0.001f) return value;
        return std::signbit(value) ? -0.001f : 0.001f;
    };
    const Vec3 safe_scale{prevent_zero_scale(scale.x), prevent_zero_scale(scale.y), prevent_zero_scale(scale.z)};
    const auto& type = reflect::Reflect<Transform>();
    const auto field = std::find_if(type.fields.begin(), type.fields.end(),
                                    [](const reflect::FieldInfo& candidate) {
                                        return std::strcmp(candidate.name, "scale") == 0;
                                    });
    if (field == type.fields.end()) return false;
    const EntityGuid guid = id->guid;
    if (!pending_scale_before_ || pending_scale_entity_ != guid) {
        pending_scale_before_ = field->Get(transform);
        pending_scale_entity_ = guid;
    }
    const reflect::Any old_value = *pending_scale_before_;
    field->Set(transform, reflect::Any(safe_scale));
    CommitFieldEdit(context_, commands_, id->guid, GetComponentId<Transform>(), *field, transform,
                    old_value, committed);
    if (committed) {
        pending_scale_before_.reset();
        pending_scale_entity_ = {};
    }
    return true;
}

bool SceneDocument::SetCameraField(Entity entity, const std::string& field_name, const reflect::Any& value) {
    if (!world_.IsAlive(entity) || !world_.HasComponent<Camera>(entity)) return false;
    const auto* id = world_.GetComponent<IdComponent>(entity);
    auto* camera = world_.GetComponent<Camera>(entity);
    if (!id || !camera) return false;
    const auto& type = reflect::Reflect<Camera>();
    const auto field = std::find_if(type.fields.begin(), type.fields.end(), [&](const reflect::FieldInfo& candidate) {
        return field_name == candidate.name && candidate.HasFlag(reflect::Field_EditAnywhere);
    });
    if (field == type.fields.end()) return false;
    reflect::Any old_value = field->Get(camera);
    if (!field->Set(camera, value)) return false;
    CommitFieldEdit(context_, commands_, id->guid, GetComponentId<Camera>(), *field, camera, old_value, true);
    return true;
}

bool SceneDocument::SetCineCameraField(Entity entity, const std::string& field_name, const reflect::Any& value) {
    if (!world_.IsAlive(entity) || !world_.HasComponent<CineCamera>(entity)) return false;
    const auto* id = world_.GetComponent<IdComponent>(entity);
    auto* camera = world_.GetComponent<CineCamera>(entity);
    if (!id || !camera) return false;
    const auto& type = reflect::Reflect<CineCamera>();
    const auto field = std::find_if(type.fields.begin(), type.fields.end(), [&](const reflect::FieldInfo& candidate) {
        return field_name == candidate.name && candidate.HasFlag(reflect::Field_EditAnywhere);
    });
    if (field == type.fields.end()) return false;
    reflect::Any old_value = field->Get(camera);
    if (!field->Set(camera, value)) return false;
    CommitFieldEdit(context_, commands_, id->guid, GetComponentId<CineCamera>(), *field, camera, old_value, true);
    return true;
}

bool SceneDocument::Undo() { return commands_.Undo(context_); }
bool SceneDocument::Redo() { return commands_.Redo(context_); }
void SceneDocument::Play() { play_session_.Play(context_, commands_); }
void SceneDocument::Pause() { play_session_.Pause(); }
void SceneDocument::Stop() {
    play_session_.Stop(context_, commands_);
    pending_position_before_.reset();
    pending_position_entity_ = {};
    pending_rotation_before_.reset();
    pending_rotation_entity_ = {};
}

} // namespace aether::editor
