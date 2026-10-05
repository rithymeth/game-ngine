#include "gameplay/gameplay_debugger_panel.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

namespace aether::editor {

void GameplayDebuggerPanel::Draw() {
    if (doc_.GetWorld() == nullptr) {
        ImGui::TextDisabled("No world to show: play the game, or open a level.");
        return;
    }
    if (ImGui::InputTextWithHint("##filter", "Filter: entity, attribute, effect, ability, tag", &filter_)) doc_.SetFilter(filter_);
    ImGui::SameLine();
    ImGui::BeginDisabled(doc_.Selected().IsNull() && !selected_only_);
    ImGui::Checkbox("Selected only", &selected_only_);
    ImGui::EndDisabled();
    doc_.Rebuild();
    ImGui::TextDisabled("%zu of %zu entities with gameplay data", doc_.Entities().size(), doc_.TotalEntities());
    if (!message_.empty()) ImGui::TextUnformatted(message_.c_str());
    const std::vector<DebugEntityRows> entities = doc_.Entities(); // actions below rebuild next frame
    for (const DebugEntityRows& rows : entities) {
        ImGui::PushID(static_cast<int>(rows.entity.index));
        if (ImGui::CollapsingHeader(rows.label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!rows.attributes.empty() && ImGui::BeginTable("attrs", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                for (const char* h : {"Attribute", "Base", "Current", "Min", "Max", "Modifiers"}) ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                for (const DebugAttributeRow& a : rows.attributes) {
                    ImGui::PushID(a.name.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(a.name.c_str());
                    ImGui::TableNextColumn();
                    f32 base = a.base;
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::InputFloat("##base", &base, 0.0f, 0.0f, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) message_ = doc_.SetBase(rows.entity, a.name, base);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", a.current);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", a.min);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", a.max);
                    ImGui::TableNextColumn();
                    if (!a.Modified()) {
                        ImGui::TextDisabled("none");
                    } else if (a.has_override) {
                        ImGui::Text("override %.2f", a.override_value);
                    } else {
                        ImGui::Text("+%.2f  x%.2f", a.add, a.mul);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (!rows.effects.empty() && ImGui::BeginTable("effects", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                for (const char* h : {"Effect", "Kind", "Remaining", "Stacks", ""}) ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                for (const DebugEffectRow& e : rows.effects) {
                    ImGui::PushID(static_cast<int>(e.handle));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s%s", e.effect.c_str(), e.known ? "" : " (not in the library)");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(e.policy.c_str());
                    ImGui::TableNextColumn();
                    if (e.policy == "infinite") ImGui::TextDisabled("-");
                    else ImGui::Text("%.2f s", e.remaining);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", e.stacks);
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Remove")) message_ = doc_.RemoveEffect(rows.entity, e.handle);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (!rows.abilities.empty() && ImGui::BeginTable("abilities", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                for (const char* h : {"Ability", "State", "Elapsed", ""}) ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                for (const DebugAbilityRow& a : rows.abilities) {
                    ImGui::PushID(a.name.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(a.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(!a.active ? "ready" : a.committed ? "running (committed)" : "running");
                    ImGui::TableNextColumn();
                    if (a.active) ImGui::Text("%.2f s", a.elapsed);
                    ImGui::TableNextColumn();
                    if (a.active && ImGui::SmallButton("Cancel")) message_ = doc_.CancelAbility(rows.entity, a.handle);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            for (const DebugTagRow& t : rows.tags) {
                ImGui::PushID(t.tag.c_str());
                ImGui::BulletText("%s  x%d", t.tag.c_str(), t.count);
                ImGui::SameLine();
                if (ImGui::SmallButton("-")) message_ = doc_.RemoveTag(rows.entity, t.tag);
                ImGui::PopID();
            }
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputTextWithHint("##tag", "Add a tag, e.g. State.Stunned", &tag_text_);
            ImGui::SameLine();
            if (ImGui::SmallButton("Add tag") && !tag_text_.empty()) {
                message_ = doc_.AddTag(rows.entity, tag_text_);
                tag_text_.clear();
            }
        }
        ImGui::PopID();
    }
}

} // namespace aether::editor
