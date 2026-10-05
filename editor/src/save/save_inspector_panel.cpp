#include "save_inspector_panel.h"

#include "ui/reflected_inspector.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "aether/platform/filesystem.h"

#include <algorithm>
#include <ctime>

namespace aether::editor {

namespace stdfs = std::filesystem;
using reflect::Json;

namespace {

std::string FormatTime(u64 unix_seconds) {
    if (unix_seconds == 0) return "unknown";
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    char buf[32];
    const std::tm* tm = std::gmtime(&t);
    if (!tm || std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC", tm) == 0) return "unknown";
    return buf;
}

std::string FormatSize(u64 bytes) {
    char buf[32];
    if (bytes < 1024) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

std::string Scalar(const Json& v) {
    if (v.is_string()) return "\"" + v.get<std::string>() + "\"";
    return v.dump();
}

// Draws `value` as a tree; `budget` counts down the nodes left to draw.
void DrawJsonNode(const std::string& name, const Json& value, usize& budget, usize depth) {
    if (budget == 0) return;
    --budget;
    const std::string label = name.empty() ? std::string("(root)") : name;
    if (value.is_object() || value.is_array()) {
        const std::string text = label + (value.is_array() ? "  [" : "  {") + std::to_string(value.size()) + (value.is_array() ? " items]" : " fields}");
        if (depth > 64) {
            ImGui::TextDisabled("%s  (too deep to show)", text.c_str());
            return;
        }
        if (ImGui::TreeNodeEx(text.c_str(), depth < 2 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            if (value.is_array()) {
                usize i = 0;
                for (const Json& item : value) DrawJsonNode("[" + std::to_string(i++) + "]", item, budget, depth + 1);
            } else {
                for (auto it = value.begin(); it != value.end(); ++it) DrawJsonNode(it.key(), it.value(), budget, depth + 1);
            }
            ImGui::TreePop();
        }
    } else {
        ImGui::BulletText("%s: %s", label.c_str(), Scalar(value).c_str());
    }
}

} // namespace

usize CountJsonNodes(const Json& value, usize limit) {
    usize n = 1;
    if (value.is_object() || value.is_array()) {
        for (const Json& child : value) {
            if (n >= limit) return n;
            n += CountJsonNodes(child, limit - n);
        }
    }
    return std::min(n, limit);
}

void SaveInspectorPanel::SetDirectory(const stdfs::path& directory) {
    directory_ = directory;
    Refresh();
}

void SaveInspectorPanel::Refresh() {
    entries_.clear();
    if (directory_.empty()) return;
    for (const std::string& name : fs::ListDirectory(directory_.string())) {
        const stdfs::path p(name);
        if (p.extension() != ".asav" && p.extension() != ".asettings") continue;
        Entry e;
        e.file = name;
        const save::envelope::EnvelopeInfo info = save::envelope::Inspect(directory_ / name);
        e.bytes = info.bytes;
        e.valid = info.ok;
        e.type = info.type;
        e.timestamp = info.timestamp;
        e.checksum_ok = info.checksum_ok;
        entries_.push_back(std::move(e));
    }
}

bool SaveInspectorPanel::DeleteOpen() {
    const bool ok = doc_.DeleteFile();
    message_ = ok ? "Deleted." : "Couldn't delete.";
    Refresh();
    return ok;
}

bool SaveInspectorPanel::CopyOpen(const std::string& name, std::string* error) {
    std::string why;
    const bool ok = doc_.CopyTo(name, &why);
    message_ = ok ? "Copied to " + name + "." : why;
    if (error) *error = why;
    Refresh();
    return ok;
}

void SaveInspectorPanel::Draw() {
    if (ImGui::Button("Refresh")) Refresh();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.IsOpen());
    if (ImGui::Button("Reload")) {
        doc_.Reload();
        Refresh();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete...")) confirm_delete_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Copy as...")) open_copy_ = true;
    ImGui::EndDisabled();
    if (!message_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", message_.c_str());
    }
    if (confirm_delete_) {
        ImGui::OpenPopup("Delete save?");
        confirm_delete_ = false;
    }
    if (ImGui::BeginPopupModal("Delete save?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete %s and its backup?", doc_.File().filename().string().c_str());
        if (ImGui::Button("Delete")) {
            DeleteOpen();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (open_copy_) {
        ImGui::OpenPopup("Copy save");
        open_copy_ = false;
    }
    if (ImGui::BeginPopupModal("Copy save", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("New name", &copy_name_);
        if (ImGui::Button("Copy") && CopyOpen(copy_name_)) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Separator();
    if (ImGui::BeginChild("##saves", ImVec2(260, 0), ImGuiChildFlags_Borders)) DrawList();
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##contents", ImVec2(0, 0))) {
        DrawHeader();
        DrawContents();
    }
    ImGui::EndChild();
}

void SaveInspectorPanel::DrawList() {
    if (directory_.empty()) {
        ImGui::TextDisabled("No folder set.");
        return;
    }
    ImGui::TextDisabled("%s", directory_.string().c_str());
    if (entries_.empty()) ImGui::TextDisabled("No saves here.");
    for (const Entry& e : entries_) {
        const stdfs::path path = directory_ / e.file;
        const bool open = doc_.IsOpen() && doc_.File() == path;
        std::string label = e.file + (e.valid ? "" : "  (unreadable)") + (e.valid && !e.checksum_ok ? "  (bad checksum)" : "");
        if (!e.valid || !e.checksum_ok) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.4f, 1.0f));
        if (ImGui::Selectable(label.c_str(), open)) doc_.Open(path);
        if (!e.valid || !e.checksum_ok) ImGui::PopStyleColor();
        if (e.valid) ImGui::TextDisabled("    %s, %s", e.type.c_str(), FormatSize(e.bytes).c_str());
    }
}

void SaveInspectorPanel::DrawHeader() {
    if (!doc_.IsOpen()) {
        ImGui::TextDisabled("Pick a save on the left.");
        return;
    }
    const save::envelope::EnvelopeInfo& info = doc_.Info();
    ImGui::Text("%s", doc_.File().filename().string().c_str());
    if (info.ok) {
        ImGui::Text("%s  |  %s v%u  |  %s  |  %s", info.kind.c_str(), info.type.c_str(), info.type_version, FormatTime(info.timestamp).c_str(),
                    FormatSize(info.bytes).c_str());
        ImGui::Text("Checksum %s  |  backup %s", info.checksum_ok ? "OK" : "BAD", doc_.HasBackup() ? "present" : "none");
    }
    for (const std::string& p : doc_.Problems()) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "! %s", p.c_str());
    ImGui::Separator();
}

void SaveInspectorPanel::DrawContents() {
    if (!doc_.IsOpen() || !doc_.Info().ok) return;
    if (!ImGui::BeginTabBar("##savetabs")) return;
    if (ImGui::BeginTabItem("Inspector")) {
        if (doc_.Instance() && doc_.Type()) {
            ImGui::TextDisabled("Read-only, as %s. Only fields flagged for the editor are shown: Raw JSON has everything.", doc_.Type()->name);
            ImGui::BeginDisabled();
            InspectObject(*doc_.Type(), doc_.Instance(), "##saved");
            ImGui::EndDisabled();
        } else {
            usize budget = kMaxTreeNodes;
            DrawJsonNode("data", doc_.Info().data, budget, 0);
            if (budget == 0) ImGui::TextDisabled("... truncated (more than %zu nodes): see Raw JSON", kMaxTreeNodes);
        }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Raw JSON")) {
        if (ImGui::Button("Copy")) ImGui::SetClipboardText(doc_.Info().raw_text.c_str());
        std::string text = doc_.Info().raw_text;
        ImGui::InputTextMultiline("##raw", &text, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

} // namespace aether::editor
