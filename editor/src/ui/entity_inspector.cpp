#include "ui/entity_inspector.h"

#include "aether/reflection/serialize.h"
#include "core/commands.h"
#include "ui/reflected_inspector.h"

#include <imgui.h>

#include <vector>

namespace aether::editor {

namespace {

bool SameValue(const reflect::TypeInfo& type, const void* a, const void* b) {
    return reflect::ToJson(type, a) == reflect::ToJson(type, b);
}

} // namespace

void InspectEntity(CommandContext& ctx, CommandStack& stack, Entity entity) {
    const EntityGuid guid = EnsureGuid(ctx.world, entity, &ctx.guids);
    // The permanent ID is identity, not editable data: a quiet line, not a card.
    ImGui::TextDisabled("ID %s", ToString(guid).c_str());

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
            InspectResult edit = InspectObject(type, component, type.name);
            if (edit.Changed()) {
                const reflect::FieldInfo& field = *edit.changed_field;
                const void* old_ptr = field.Ptr(before.Data());
                if (!SameValue(*field.type, old_ptr, field.Ptr(component))) {
                    reflect::Any new_value = field.Get(component);
                    reflect::Any old_value = reflect::Any::CopyOf(*field.type, old_ptr);
                    field.Set(component, old_value); // the command re-applies it (and fires hooks)
                    stack.Execute(ctx,
                                  std::make_unique<SetFieldCommand>(guid, id, field, std::move(old_value),
                                                                    std::move(new_value)),
                                  MergePolicy::Allow);
                }
            }
            if (edit.committed) {
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
