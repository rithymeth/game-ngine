#include "devtools/crash_reporter.h"

#include <imgui.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>

namespace aether::editor {

void CrashReporterDialog::Refresh() {
    reports_ = ListCrashReports(directory_);
    selected = 0;
    open = !reports_.empty();
}

std::string CrashReporterDialog::ReportText(usize index) const {
    if (index >= reports_.size()) return {};
    std::ifstream in(reports_[index].path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void CrashReporterDialog::Remove(usize index) {
    reports_.erase(reports_.begin() + static_cast<std::ptrdiff_t>(index));
    selected = std::clamp(selected, 0, std::max(0, static_cast<int>(reports_.size()) - 1));
    if (reports_.empty()) open = false;
}

bool CrashReporterDialog::Dismiss(usize index) {
    if (index >= reports_.size() || !MarkCrashReportSeen(reports_[index])) return false;
    Remove(index);
    return true;
}

bool CrashReporterDialog::Delete(usize index) {
    if (index >= reports_.size() || !DeleteCrashReport(reports_[index])) return false;
    Remove(index);
    return true;
}

void CrashReporterDialog::DismissAll() {
    while (!reports_.empty() && Dismiss(0)) {
    }
    reports_.clear();
    open = false;
}

void CrashReporterDialog::Draw() {
    if (!open || reports_.empty()) return;
    ImGui::SetNextWindowSize(ImVec2(900, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Crash Reporter", &open)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("%s. The reports are kept in %s.",
                       reports_.size() == 1 ? "The last run crashed" : "Earlier runs crashed", directory_.c_str());
    ImGui::BeginChild("list", ImVec2(260, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
    for (usize i = 0; i < reports_.size(); ++i) {
        const CrashReport& r = reports_[i];
        std::string when = r.Field("time");
        if (!when.empty()) {
            const std::time_t t = static_cast<std::time_t>(std::stoll(when));
            char buf[32];
            if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t))) when = buf;
        }
        const std::string label = when + "\n" + r.Field("reason") + "##" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), selected == static_cast<int>(i))) selected = static_cast<int>(i);
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("details", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
    if (selected >= 0 && selected < static_cast<int>(reports_.size())) {
        const CrashReport& r = reports_[static_cast<usize>(selected)];
        ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", r.Field("reason").c_str());
        ImGui::Text("%s %s, process %s, thread %s, address %s", r.Field("app").c_str(), r.Field("build").c_str(),
                    r.Field("pid").c_str(), r.Field("thread").c_str(), r.Field("address").c_str());
        bool any_context = false;
        for (const auto& [key, value] : r.fields)
            if (key.rfind("context.", 0) == 0) {
                if (!any_context) ImGui::SeparatorText("Context");
                any_context = true;
                ImGui::BulletText("%s: %s", key.substr(8).c_str(), value.c_str());
            }
        if (ImGui::CollapsingHeader("Backtrace", ImGuiTreeNodeFlags_DefaultOpen))
            for (const std::string& line : r.backtrace) ImGui::TextUnformatted(line.c_str());
        if (ImGui::CollapsingHeader("Log", ImGuiTreeNodeFlags_DefaultOpen))
            for (const std::string& line : r.log) ImGui::TextUnformatted(line.c_str());
    }
    ImGui::EndChild();
    const usize index = static_cast<usize>(std::max(selected, 0));
    if (ImGui::Button("Copy report")) ImGui::SetClipboardText(ReportText(index).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Dismiss")) Dismiss(index);
    ImGui::SameLine();
    if (ImGui::Button("Delete")) Delete(index);
    ImGui::SameLine();
    if (ImGui::Button("Dismiss all")) DismissAll();
    ImGui::End();
}

} // namespace aether::editor
