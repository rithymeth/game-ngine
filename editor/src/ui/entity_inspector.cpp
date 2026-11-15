#include "ui/entity_inspector.h"

#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"
#include "core/commands.h"
#include "ui/reflected_inspector.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace aether::editor {

namespace {

bool IsInspectable(ComponentId id) {
    const ComponentInfo& info = GetComponentInfo(id);
    return info.reflected != nullptr && HasInspectableFields(*info.reflected) && id != GetComponentId<IdComponent>();
}

} // namespace

void InspectEntity(CommandContext& ctx, CommandStack& stack, Entity entity) {
    const EntityGuid guid = EnsureGuid(ctx.world, entity, &ctx.guids);
    // The permanent ID is identity, not editable data: a quiet line, not a card.
    ImGui::TextDisabled("ID %s", ToString(guid).c_str());

    // Part of a prefab instance: overridden fields are marked (needs the
    // prefab's data to tell, via CommandContext::find_prefab).
    // Only handles and ids are kept across the loop below: adding or removing
    // a component moves component storage.
    const Entity instance_root = ctx.find_prefab ? FindInstanceRoot(ctx.world, ctx.guids, entity) : kNullEntity;
    const PrefabLink* link = instance_root.IsNull() ? nullptr : ctx.world.GetComponent<PrefabLink>(entity);
    const bool in_instance = link != nullptr;
    const PrefabLocalId local_id = in_instance ? link->local_id : 0;
    if (in_instance) {
        const PrefabInstance* instance = ctx.world.GetComponent<PrefabInstance>(instance_root);
        const usize count = static_cast<usize>(std::count_if(
            instance->overrides.begin(), instance->overrides.end(),
            [&](const PropertyOverride& o) { return o.entity == local_id; }));
        ImGui::TextDisabled("Prefab instance, %zu override(s) here", count);
        if (count > 0 && ImGui::SmallButton("Revert All")) {
            stack.Execute(ctx, std::make_unique<RevertPrefabOverrideCommand>(guid, kInvalidComponentId, ""));
            return; // resolving may have changed the entity's components
        }
    }

    std::vector<const reflect::TypeInfo*> addable;
    std::vector<ComponentId> addable_ids;
    for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
        const ComponentInfo& info = GetComponentInfo(id);
        if (info.reflected == nullptr || !HasInspectableFields(*info.reflected) ||
            id == GetComponentId<IdComponent>()) {
            continue;
        }
        if (!ctx.world.HasComponentRaw(entity, id)) {
            addable.push_back(info.reflected);
            addable_ids.push_back(id);
            continue;
        }

        const reflect::TypeInfo& type = *info.reflected;
        ImGui::PushID(static_cast<int>(id));
        const bool open = ImGui::CollapsingHeader(type.name, ImGuiTreeNodeFlags_DefaultOpen);
        bool remove = false;
        if (ImGui::BeginPopupContextItem("component_menu")) {
            remove = ImGui::MenuItem("Remove Component");
            ImGui::EndPopup();
        }
        if (remove) {
            stack.Execute(ctx, std::make_unique<RemoveComponentCommand>(guid, id));
            ImGui::PopID();
            continue; // the component no longer exists
        }
        if (open) {
            void* component = ctx.world.GetComponentRaw(entity, id);
            // InspectObject edits in place; keep the pre-edit value so the
            // change can be turned into a command instead.
            reflect::Any before = reflect::Any::CopyOf(type, component);
            InspectOptions options;
            if (in_instance && !(entity == instance_root && id == GetComponentId<Transform>())) {
                options.is_overridden = [&](const reflect::FieldInfo& field) {
                    const PrefabInstance* instance = ctx.world.GetComponent<PrefabInstance>(instance_root);
                    return instance != nullptr && IsFieldOverridden(*instance, local_id, type.name, field.name);
                };
            }
            InspectResult edit = InspectObject(type, component, type.name, options);
            if (edit.revert_field != nullptr) {
                stack.Execute(ctx, std::make_unique<RevertPrefabOverrideCommand>(guid, id, edit.revert_field->name));
                ImGui::PopID();
                continue; // resolving may have changed the entity's components
            }
            if (edit.Changed()) {
                const reflect::FieldInfo& field = *edit.changed_field;
                // Picking another model sets the permanent reference; the path the renderer loads from follows it in the same undo step.
                const bool model_pick = id == GetComponentId<ModelRenderer>() && std::strcmp(field.name, "model") == 0;
                if (model_pick) stack.BeginTransaction("Change model");
                CommitFieldEdit(ctx, stack, guid, id, field, component,
                                reflect::Any::CopyOf(*field.type, field.Ptr(before.Data())), edit.committed);
                if (model_pick) {
                    auto* renderer = ctx.world.GetComponent<ModelRenderer>(entity);
                    const std::string path = renderer != nullptr ? AssetNameOf("Model", renderer->model.guid) : std::string();
                    const auto path_field = std::find_if(type.fields.begin(), type.fields.end(), [](const reflect::FieldInfo& f) { return std::strcmp(f.name, "asset_path") == 0; });
                    if (renderer != nullptr && !path.empty() && path != renderer->asset_path && path_field != type.fields.end()) {
                        const reflect::Any old_path = path_field->Get(renderer);
                        SetModelPath(*renderer, path);
                        CommitFieldEdit(ctx, stack, guid, id, *path_field, renderer, old_path, true);
                    }
                    stack.EndTransaction();
                }
            } else if (edit.committed) {
                stack.BreakMergeChain();
            }
        }
        ImGui::PopID();
    }

    int picked = AddComponentButton(addable);
    if (picked >= 0) {
        stack.Execute(ctx, std::make_unique<AddComponentCommand>(guid, addable_ids[static_cast<usize>(picked)]));
    }
}

std::vector<ComponentId> SharedComponents(const World& world, std::span<const Entity> entities) {
    std::vector<ComponentId> shared;
    if (entities.empty()) return shared;
    for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
        if (!IsInspectable(id)) continue;
        const bool all_have_it = std::all_of(entities.begin(), entities.end(), [&](Entity entity) {
            return world.HasComponentRaw(entity, id);
        });
        if (all_have_it) shared.push_back(id);
    }
    return shared;
}

void InspectEntities(CommandContext& ctx, CommandStack& stack, std::span<const Entity> entities) {
    if (entities.empty()) return;
    if (entities.size() == 1) {
        InspectEntity(ctx, stack, entities[0]);
        return;
    }
    std::vector<EntityGuid> guids;
    guids.reserve(entities.size());
    for (const Entity entity : entities) guids.push_back(EnsureGuid(ctx.world, entity, &ctx.guids));
    ImGui::TextDisabled("%zu entities selected", entities.size());

    std::vector<ComponentId> shared = SharedComponents(ctx.world, entities);
    std::vector<const reflect::TypeInfo*> addable;
    std::vector<ComponentId> addable_ids;
    for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
        if (!IsInspectable(id) || std::find(shared.begin(), shared.end(), id) != shared.end()) continue;
        addable.push_back(GetComponentInfo(id).reflected);
        addable_ids.push_back(id);
    }

    for (const ComponentId id : shared) {
        const reflect::TypeInfo& type = *GetComponentInfo(id).reflected;
        ImGui::PushID(static_cast<int>(id));
        const bool open = ImGui::CollapsingHeader(type.name, ImGuiTreeNodeFlags_DefaultOpen);
        bool remove = false;
        if (ImGui::BeginPopupContextItem("component_menu")) {
            remove = ImGui::MenuItem("Remove Component");
            ImGui::EndPopup();
        }
        if (remove) {
            stack.BeginTransaction(std::string("Remove ") + type.name + " from " +
                                   std::to_string(entities.size()) + " entities");
            for (const EntityGuid& guid : guids) stack.Execute(ctx, std::make_unique<RemoveComponentCommand>(guid, id));
            stack.EndTransaction();
            ImGui::PopID();
            continue; // the component is gone from every selected entity
        }
        if (open) {
            std::vector<void*> data;
            std::vector<reflect::Any> before;
            for (const Entity entity : entities) {
                void* component = ctx.world.GetComponentRaw(entity, id);
                if (component == nullptr) break;
                before.push_back(reflect::Any::CopyOf(type, component));
                data.push_back(component);
            }
            if (data.size() == entities.size()) {
                InspectResult edit = InspectObjects(type, data, type.name);
                if (edit.Changed()) {
                    const reflect::FieldInfo& field = *edit.changed_field;
                    const reflect::Any new_value = field.Get(data.front());
                    // The old values are the field's, taken from each entity's component before it was drawn.
                    std::vector<reflect::Any> old_values;
                    old_values.reserve(before.size());
                    for (const reflect::Any& whole : before) {
                        old_values.push_back(reflect::Any::CopyOf(*field.type, field.Ptr(whole.Data())));
                    }
                    for (usize i = 0; i < data.size(); ++i) field.Set(data[i], old_values[i]); // the command re-applies it
                    stack.Execute(ctx,
                                  std::make_unique<SetFieldOnEntitiesCommand>(id, field, guids,
                                                                              std::move(old_values), new_value),
                                  MergePolicy::Allow);
                } else if (edit.committed) {
                    stack.BreakMergeChain();
                }
            }
        }
        ImGui::PopID();
    }

    const int picked = AddComponentButton(addable);
    if (picked >= 0) {
        const ComponentId id = addable_ids[static_cast<usize>(picked)];
        stack.BeginTransaction(std::string("Add ") + GetComponentInfo(id).reflected->name + " to " +
                               std::to_string(entities.size()) + " entities");
        for (const EntityGuid& guid : guids) {
            const Entity live = ctx.guids.Find(ctx.world, guid);
            if (!live.IsNull() && !ctx.world.HasComponentRaw(live, id)) {
                stack.Execute(ctx, std::make_unique<AddComponentCommand>(guid, id));
            }
        }
        stack.EndTransaction();
    }
}

} // namespace aether::editor
