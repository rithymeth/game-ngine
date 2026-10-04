#include "packaging/plugins_panel.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace aether::editor {

namespace stdfs = std::filesystem;

namespace {

bool ContainsNoCase(const std::string& text, const std::string& part) {
    if (part.empty()) return true;
    const auto it = std::search(text.begin(), text.end(), part.begin(), part.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != text.end();
}

} // namespace

PluginsPanel::PluginsPanel(stdfs::path project_file) : project_file_(std::move(project_file)) { Refresh(); }

bool PluginsPanel::Resolve() {
    warnings_.clear();
    error_.clear();
    manager_ = plugin::PluginManager();
    if (const stdfs::path engine = plugin::PluginManager::EnginePluginsDir(); !engine.empty()) {
        manager_.AddSearchPath(engine, plugin::PluginSource::Engine);
    }
    manager_.AddSearchPath(ProjectPaths::ForFile(project_file_).root / "Plugins", plugin::PluginSource::Project);
    warnings_ = manager_.Discover();
    return manager_.Resolve(settings_.plugins, &error_, &warnings_);
}

bool PluginsPanel::Refresh() {
    std::string why;
    if (!LoadProject(project_file_, settings_, &why)) {
        error_ = why;
        return false;
    }
    return Resolve();
}

bool PluginsPanel::SetEnabled(const std::string& name, bool enabled, std::string* error) {
    const plugin::PluginInfo* info = manager_.Find(name);
    if (!info) {
        if (error) *error = "No plugin named '" + name + "'";
        return false;
    }
    std::vector<std::string>& list = settings_.plugins;
    if (!enabled && info->enabled && info->enabled_reason != "project") {
        if (error) *error = "'" + name + "' stays enabled: " + info->enabled_reason;
        return false;
    }
    const auto it = std::find(list.begin(), list.end(), name);
    if (enabled == (it != list.end())) return true;
    const std::vector<std::string> before = list;
    if (enabled) {
        list.push_back(name);
    } else {
        list.erase(it);
    }
    if (!Resolve()) {
        // It doesn't resolve (a missing dependency, ...): keep the project as it was.
        const std::string why = error_;
        list = before;
        Resolve();
        error_ = why;
        if (error) *error = why;
        return false;
    }
    std::string why;
    if (!SaveProject(project_file_, settings_, &why)) {
        list = before;
        Resolve();
        error_ = why;
        if (error) *error = why;
        return false;
    }
    return true;
}

bool PluginsPanel::CreatePlugin(const std::string& name, std::string* error) {
    std::string why;
    if (!plugin::CreatePluginScaffold(ProjectPaths::ForFile(project_file_).root / "Plugins", name, nullptr, &why)) {
        error_ = why;
        if (error) *error = why;
        return false;
    }
    Refresh();
    return true;
}

void PluginsPanel::Draw() {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s", filter_.c_str());
    ImGui::SetNextItemWidth(220);
    if (ImGui::InputTextWithHint("##filter", "Search plugins", buffer, sizeof(buffer))) filter_ = buffer;
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) Refresh();
    ImGui::SameLine();
    std::snprintf(buffer, sizeof(buffer), "%s", new_name_.c_str());
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputTextWithHint("##new", "NewPluginName", buffer, sizeof(buffer))) new_name_ = buffer;
    ImGui::SameLine();
    ImGui::BeginDisabled(new_name_.empty());
    if (ImGui::Button("New Plugin") && CreatePlugin(new_name_)) new_name_.clear();
    ImGui::EndDisabled();
    if (!error_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", error_.c_str());
    for (const std::string& w : warnings_) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", w.c_str());
    ImGui::Separator();

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##plugins", 5, kFlags)) return;
    ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 28);
    ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthFixed, 200);
    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableSetupColumn("Modules", ImGuiTableColumnFlags_WidthFixed, 160);
    ImGui::TableSetupColumn("About", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const plugin::PluginInfo& p : manager_.Plugins()) {
        const plugin::PluginDescriptor& d = p.descriptor;
        if (!ContainsNoCase(d.name + " " + d.friendly_name + " " + d.description + " " + d.category, filter_)) continue;
        ImGui::PushID(d.name.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        bool on = p.enabled;
        const bool locked = p.enabled && p.enabled_reason != "project";
        ImGui::BeginDisabled(locked);
        if (ImGui::Checkbox("##on", &on)) SetEnabled(d.name, on);
        ImGui::EndDisabled();
        if (locked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Enabled: %s", p.enabled_reason.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(d.friendly_name.c_str());
        ImGui::TextDisabled("%s %s, %s", d.name.c_str(), d.version.c_str(),
                            p.source == plugin::PluginSource::Engine ? "engine" : "project");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(d.category.c_str());
        ImGui::TableNextColumn();
        for (const plugin::ModuleDesc& m : d.modules) {
            const bool built = plugin::ModuleRegistry::Get().Has(m.name);
            ImGui::TextColored(built ? ImVec4(0.5f, 0.9f, 0.5f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1), "%s %s (%s)",
                               built ? "+" : "-", m.name.c_str(), m.type == plugin::ModuleType::Runtime ? "runtime" : "editor");
        }
        if (d.modules.empty()) ImGui::TextDisabled("content only");
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", d.description.empty() ? "(no description)" : d.description.c_str());
        if (!d.dependencies.empty()) {
            std::string needs = "Needs:";
            for (const plugin::PluginDependency& dep : d.dependencies) {
                needs += " " + dep.name + (dep.min_version.empty() ? "" : " " + dep.min_version) + (dep.optional ? " (optional)" : "");
            }
            ImGui::TextDisabled("%s", needs.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

} // namespace aether::editor
