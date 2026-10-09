#include "core/scene_document.h"

#include "aether/ecs/archetype.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"
#include "core/commands.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
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

SceneDocument::SceneDocument() : context_(world_, guids_) {}

void SceneDocument::Reset() {
    if (!play_session_.IsEditing()) play_session_.Stop(context_, commands_);
    world_ = World{};
    guids_.Clear();
    commands_.Clear();
    file_.clear();
    pending_position_before_.reset();
    pending_position_entity_ = {};
    pending_rotation_before_.reset();
    pending_rotation_entity_ = {};
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
    const SceneLoadOptions editor_load_options{.reject_unknown_components = true};
    const bool loaded = file.extension() == ".ascene"
        ? LoadSceneJson(candidate, path, editor_load_options)
        : LoadScene(candidate, path, editor_load_options);
    if (!loaded) {
        if (error) *error = "The scene couldn't be loaded safely. Check the Output panel for the unsupported component.";
        return false;
    }

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
    std::unordered_set<std::string> used_names;
    for (Entity entity : entities) {
        if (const auto* name = candidate.GetComponent<EntityName>(entity); name && !name->value.empty())
            used_names.insert(name->value);
    }
    for (Entity entity : entities) {
        auto* existing_name = candidate.GetComponent<EntityName>(entity);
        const std::string legacy_placeholder = "Entity " + std::to_string(entity.index);
        if (existing_name && !existing_name->value.empty() && existing_name->value != legacy_placeholder) continue;
        const std::string base = SuggestedEntityName(candidate, entity);
        std::string unique = base;
        for (u32 suffix = 2; used_names.contains(unique); ++suffix)
            unique = base + " " + std::to_string(suffix);
        if (existing_name) existing_name->value = unique;
        else candidate.AddComponent(entity, EntityName{unique});
        used_names.insert(std::move(unique));
    }
    world_ = std::move(candidate);
    guids_ = std::move(candidate_guids);
    commands_.Clear();
    pending_position_before_.reset();
    pending_position_entity_ = {};
    pending_rotation_before_.reset();
    pending_rotation_entity_ = {};
    file_ = file;
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
    const bool saved = target.extension() == ".ascene" ? SaveSceneJson(world_, path) : SaveScene(world_, path);
    if (!saved) {
        if (error) *error = "The scene couldn't be saved.";
        return false;
    }
    file_ = target;
    commands_.MarkSaved();
    return true;
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
