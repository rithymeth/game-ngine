#include "ui/entity_inspector.h"

#include "aether/reflection/serialize.h"
#include "core/commands.h"
#include "ui/reflected_inspector.h"

#include <imgui.h>

#include <algorithm>
#include <vector>

namespace aether::editor {

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
                CommitFieldEdit(ctx, stack, guid, id, field, component,
                                reflect::Any::CopyOf(*field.type, field.Ptr(before.Data())), edit.committed);
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

} // namespace aether::editor
