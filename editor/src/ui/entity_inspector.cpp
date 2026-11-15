#include "ui/entity_inspector.h"

#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"
#include "core/commands.h"
#include "ui/reflected_inspector.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether::editor {

namespace {

bool IsInspectable(ComponentId id) {
    const ComponentInfo& info = GetComponentInfo(id);
    return info.reflected != nullptr && HasInspectableFields(*info.reflected) && id != GetComponentId<IdComponent>();
}

// Writes `source`'s copied fields into `component` on every entity as one undo step: one SetFieldOnEntitiesCommand per
// field that differs anywhere, each carrying every entity's own old value.
bool ApplyComponentValues(CommandContext& ctx, CommandStack& stack, std::span<const Entity> entities, ComponentId id,
                          const void* source, const std::string& label) {
    const reflect::TypeInfo* type = GetComponentInfo(id).reflected;
    if (type == nullptr || source == nullptr || entities.empty()) return false;
    std::vector<void*> data;
    for (const Entity entity : entities) {
        void* component = ctx.world.GetComponentRaw(entity, id);
        if (component == nullptr) return false;
        data.push_back(component);
    }
    struct Change {
        const reflect::FieldInfo* field;
        std::vector<reflect::Any> old_values;
    };
    std::vector<Change> changes;
    for (const reflect::FieldInfo& field : type->fields) {
        if (!IsCopiedField(field)) continue;
        const bool differs = std::any_of(data.begin(), data.end(), [&](const void* component) {
            return !ValuesEqual(*field.type, field.Ptr(component), field.Ptr(source));
        });
        if (!differs) continue;
        Change change{&field, {}};
        for (const void* component : data) change.old_values.push_back(field.Get(component));
        changes.push_back(std::move(change));
    }
    if (changes.empty()) return false;

    std::vector<EntityGuid> guids;
    for (const Entity entity : entities) guids.push_back(EnsureGuid(ctx.world, entity, &ctx.guids));
    stack.BreakMergeChain();
    stack.BeginTransaction(label);
    for (Change& change : changes) {
        stack.Execute(ctx, std::make_unique<SetFieldOnEntitiesCommand>(
                               id, *change.field, guids, std::move(change.old_values),
                               reflect::Any::CopyOf(*change.field->type, change.field->Ptr(source))));
    }
    stack.EndTransaction();
    stack.BreakMergeChain();
    return true;
}

std::string SelectionSuffix(usize count) {
    return count > 1 ? " on " + std::to_string(count) + " entities" : std::string();
}

enum class CardAction { None, Edited, Remove, Revert };

// The card header: a collapsing header with a «...» button at its right edge; the button and a right-click open the
// same menu, whose edits act on every entity in `entities`.
CardAction DrawComponentCard(CommandContext& ctx, CommandStack& stack, std::span<const Entity> entities,
                             ComponentId id, bool can_revert, bool& open) {
    const reflect::TypeInfo& type = *GetComponentInfo(id).reflected;
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    open = ImGui::CollapsingHeader(type.name, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
    ImGui::OpenPopupOnItemClick("component_menu", ImGuiPopupFlags_MouseButtonRight);
    const float button = ImGui::CalcTextSize("...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(right - button);
    // Out of keyboard navigation: a focus request landing on it would click it and open the menu over the fields.
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    if (ImGui::SmallButton("...")) ImGui::OpenPopup("component_menu");
    ImGui::PopItemFlag();

    CardAction action = CardAction::None;
    if (ImGui::BeginPopup("component_menu")) {
        if (ImGui::MenuItem("Copy Component")) CopyComponent(ctx.world, entities.front(), id);
        if (ImGui::MenuItem("Paste Component Values", nullptr, false, ObjectClipboardFits(type)) &&
            PasteComponentValues(ctx, stack, entities, id)) {
            action = CardAction::Edited;
        }
        if (ImGui::MenuItem("Reset Component") && ResetComponent(ctx, stack, entities, id)) action = CardAction::Edited;
        if (can_revert && ImGui::MenuItem("Revert Component to Prefab")) action = CardAction::Revert;
        ImGui::Separator();
        if (ImGui::MenuItem("Remove Component")) action = CardAction::Remove;
        ImGui::EndPopup();
    }
    return action;
}

// The JSON at "stats.max", "items[2].tint" or "" (the whole value) inside `root`, or null if any step is missing.
const nlohmann::json* JsonAtPath(const nlohmann::json& root, const std::string& path) {
    const nlohmann::json* node = &root;
    usize i = 0;
    while (i < path.size()) {
        if (path[i] == '.') {
            ++i;
            continue;
        }
        if (path[i] == '[') {
            const usize close = path.find(']', i);
            if (close == std::string::npos || close == i + 1 || close - i > 7 || !node->is_array()) return nullptr;
            usize index = 0;
            for (usize d = i + 1; d < close; ++d) {
                if (path[d] < '0' || path[d] > '9') return nullptr;
                index = index * 10 + static_cast<usize>(path[d] - '0');
            }
            if (index >= node->size()) return nullptr;
            node = &(*node)[index];
            i = close + 1;
            continue;
        }
        usize end = i;
        while (end < path.size() && path[end] != '.' && path[end] != '[') ++end;
        if (!node->is_object()) return nullptr;
        const auto it = node->find(path.substr(i, end - i));
        if (it == node->end()) return nullptr;
        node = &*it;
        i = end;
    }
    return node;
}

std::string Shortened(const std::string& text) {
    constexpr usize kMax = 48;
    return text.size() <= kMax ? text : text.substr(0, kMax - 3) + "...";
}

// The instance-wide override list: every override on every entity of the instance, each with the prefab's value, the
// instance's, and a Revert of its own. True when an action ran (resolving may have changed the entity's components).
bool DrawInstanceOverrides(CommandContext& ctx, CommandStack& stack, Entity entity) {
    const std::vector<InstanceOverride> rows = InstanceOverrides(ctx, entity);
    if (rows.empty()) return false;
    if (!ImGui::TreeNode("instance_overrides", "Overrides in this instance (%zu)", rows.size())) return false;
    bool acted = false;
    if (ImGui::SmallButton("Revert Whole Instance")) acted = RevertWholeInstance(ctx, stack, entity);
    for (usize i = 0; i < rows.size() && !acted; ++i) {
        const InstanceOverride& row = rows[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton("Revert")) acted = RevertInstanceOverride(ctx, stack, entity, row);
        ImGui::SameLine();
        const std::string where = row.entity_name + " / " + row.component + (row.field_path.empty() ? "" : "." + row.field_path);
        if (row.orphaned) {
            ImGui::TextDisabled("%s: no longer in the prefab", where.c_str());
        } else {
            if (row.entity == entity) ImGui::TextColored(ImVec4(0.35f, 0.65f, 1.0f, 1.0f), "%s", where.c_str());
            else ImGui::TextUnformatted(where.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s -> %s", Shortened(row.prefab_value).c_str(), Shortened(row.value).c_str());
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
    return acted;
}

} // namespace

std::vector<InstanceOverride> InstanceOverrides(const CommandContext& ctx, Entity entity) {
    if (!ctx.find_prefab || !ctx.world.IsAlive(entity)) return {};
    const Entity root = FindInstanceRoot(ctx.world, ctx.guids, entity);
    const PrefabInstance* instance = root.IsNull() ? nullptr : ctx.world.GetComponent<PrefabInstance>(root);
    if (instance == nullptr) return {};
    const PrefabData* prefab = ctx.find_prefab(instance->source.guid);
    const IdComponent* root_id = ctx.world.GetComponent<IdComponent>(root);
    if (prefab == nullptr || root_id == nullptr) return {};

    std::unordered_map<PrefabLocalId, Entity> members;
    const ComponentId link_id = GetComponentId<PrefabLink>();
    ctx.world.ForEachArchetype([&](const Archetype& archetype) {
        if (!archetype.Mask().test(link_id)) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* entities = archetype.EntityArray(c);
            for (u32 i = 0; i < archetype.ChunkEntityCount(c); ++i) {
                const PrefabLink* link = ctx.world.GetComponent<PrefabLink>(entities[i]);
                if (link != nullptr && link->instance == root_id->guid) members[link->local_id] = entities[i];
            }
        }
    });

    std::vector<InstanceOverride> rows;
    for (const PropertyOverride& o : instance->overrides) {
        InstanceOverride row;
        row.local_id = o.entity;
        row.component = o.component;
        row.field_path = o.field_path;
        row.value = o.value;
        const auto member = members.find(o.entity);
        row.entity = member == members.end() ? kNullEntity : member->second;
        const EntityName* name = row.entity.IsNull() ? nullptr : ctx.world.GetComponent<EntityName>(row.entity);
        row.entity_name = name != nullptr && !name->value.empty() ? name->value : "Entity #" + std::to_string(o.entity);
        const PrefabEntity* source = prefab->Find(o.entity);
        const nlohmann::json* value = nullptr;
        if (source != nullptr) {
            const auto component = source->components.find(o.component);
            if (component != source->components.end()) value = JsonAtPath(*component, o.field_path);
        }
        row.orphaned = row.entity.IsNull() || value == nullptr;
        if (value != nullptr) row.prefab_value = value->dump();
        rows.push_back(std::move(row));
    }
    std::stable_sort(rows.begin(), rows.end(), [](const InstanceOverride& a, const InstanceOverride& b) { return a.local_id < b.local_id; });
    return rows;
}

bool RevertInstanceOverride(CommandContext& ctx, CommandStack& stack, Entity entity, const InstanceOverride& row) {
    if (!ctx.world.IsAlive(entity)) return false;
    const Entity root = FindInstanceRoot(ctx.world, ctx.guids, entity);
    const PrefabInstance* instance = root.IsNull() ? nullptr : ctx.world.GetComponent<PrefabInstance>(root);
    if (instance == nullptr || row.component.empty()) return false;
    const bool exists = std::any_of(instance->overrides.begin(), instance->overrides.end(), [&](const PropertyOverride& o) {
        return o.entity == row.local_id && o.component == row.component && o.field_path == row.field_path;
    });
    if (!exists) return false;
    const EntityGuid guid = EnsureGuid(ctx.world, root, &ctx.guids);
    stack.BreakMergeChain();
    stack.Execute(ctx, RevertPrefabOverrideCommand::ByName(guid, row.local_id, row.component, row.field_path));
    stack.BreakMergeChain();
    return true;
}

bool RevertWholeInstance(CommandContext& ctx, CommandStack& stack, Entity entity) {
    if (!ctx.world.IsAlive(entity)) return false;
    const Entity root = FindInstanceRoot(ctx.world, ctx.guids, entity);
    const PrefabInstance* instance = root.IsNull() ? nullptr : ctx.world.GetComponent<PrefabInstance>(root);
    if (instance == nullptr || instance->overrides.empty()) return false;
    const EntityGuid guid = EnsureGuid(ctx.world, root, &ctx.guids);
    stack.BreakMergeChain();
    stack.Execute(ctx, std::make_unique<RevertPrefabOverrideCommand>(guid, PrefabLocalId{0}, kInvalidComponentId, std::string()));
    stack.BreakMergeChain();
    return true;
}

void CopyComponent(const World& world, Entity entity, ComponentId component) {
    const reflect::TypeInfo* type = GetComponentInfo(component).reflected;
    const void* data = world.GetComponentRaw(entity, component);
    if (type != nullptr && data != nullptr) CopyObjectValues(*type, data);
}

bool PasteComponentValues(CommandContext& ctx, CommandStack& stack, std::span<const Entity> entities,
                          ComponentId component) {
    const reflect::TypeInfo* type = GetComponentInfo(component).reflected;
    if (type == nullptr) return false;
    return ApplyComponentValues(ctx, stack, entities, component, ObjectClipboardData(*type),
                                std::string("Paste ") + type->name + " values" + SelectionSuffix(entities.size()));
}

bool ResetComponent(CommandContext& ctx, CommandStack& stack, std::span<const Entity> entities, ComponentId component) {
    const reflect::TypeInfo* type = GetComponentInfo(component).reflected;
    if (type == nullptr) return false;
    const reflect::Any defaults = reflect::Any::DefaultOf(*type);
    if (!defaults.HasValue()) return false;
    return ApplyComponentValues(ctx, stack, entities, component, defaults.Data(),
                                std::string("Reset ") + type->name + SelectionSuffix(entities.size()));
}

void InspectEntity(CommandContext& ctx, CommandStack& stack, Entity entity, std::string_view search) {
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
        if (DrawInstanceOverrides(ctx, stack, entity)) return;
    }

    std::vector<const reflect::TypeInfo*> addable;
    std::vector<ComponentId> addable_ids;
    usize matching = 0;
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
        if (!HasMatchingFields(type, search)) continue;
        ++matching;
        ImGui::PushID(static_cast<int>(id));
        bool can_revert = false;
        if (in_instance) {
            const PrefabInstance* instance = ctx.world.GetComponent<PrefabInstance>(instance_root);
            can_revert = instance != nullptr && std::any_of(instance->overrides.begin(), instance->overrides.end(),
                                                            [&](const PropertyOverride& o) {
                                                                return o.entity == local_id && o.component == type.name;
                                                            });
        }
        bool open = false;
        const Entity one[] = {entity};
        const CardAction action = DrawComponentCard(ctx, stack, one, id, can_revert, open);
        if (action == CardAction::Remove) {
            stack.Execute(ctx, std::make_unique<RemoveComponentCommand>(guid, id));
            ImGui::PopID();
            continue; // the component no longer exists
        }
        if (action == CardAction::Revert) {
            stack.Execute(ctx, std::make_unique<RevertPrefabOverrideCommand>(guid, id, ""));
            ImGui::PopID();
            return; // resolving may have changed the entity's components
        }
        if (open) {
            void* component = ctx.world.GetComponentRaw(entity, id);
            // InspectObject edits in place; keep the pre-edit value so the
            // change can be turned into a command instead.
            reflect::Any before = reflect::Any::CopyOf(type, component);
            InspectOptions options;
            options.default_value = [&type](const reflect::FieldInfo& field) { return DefaultFieldValue(type, field); };
            options.search = search;
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

    if (search.empty()) {
        int picked = AddComponentButton(addable);
        if (picked >= 0) {
            stack.Execute(ctx, std::make_unique<AddComponentCommand>(guid, addable_ids[static_cast<usize>(picked)]));
        }
    } else if (matching == 0) {
        ImGui::TextDisabled("No properties match \"%.*s\".", static_cast<int>(search.size()), search.data());
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

void InspectEntities(CommandContext& ctx, CommandStack& stack, std::span<const Entity> entities,
                     std::string_view search) {
    if (entities.empty()) return;
    if (entities.size() == 1) {
        InspectEntity(ctx, stack, entities[0], search);
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

    usize matching = 0;
    for (const ComponentId id : shared) {
        const reflect::TypeInfo& type = *GetComponentInfo(id).reflected;
        if (!HasMatchingFields(type, search)) continue;
        ++matching;
        ImGui::PushID(static_cast<int>(id));
        bool open = false;
        if (DrawComponentCard(ctx, stack, entities, id, /*can_revert=*/false, open) == CardAction::Remove) {
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
                InspectOptions options;
                options.default_value = [&type](const reflect::FieldInfo& field) { return DefaultFieldValue(type, field); };
                options.search = search;
                InspectResult edit = InspectObjects(type, data, type.name, options);
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

    if (!search.empty()) {
        if (matching == 0) ImGui::TextDisabled("No properties match \"%.*s\".", static_cast<int>(search.size()), search.data());
        return;
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
