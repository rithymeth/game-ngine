#include "devtools/console_panel.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace aether::editor {

namespace {

ImVec4 LevelColor(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return ImVec4(0.6f, 0.6f, 0.6f, 1);
    case LogLevel::Warn: return ImVec4(1.0f, 0.8f, 0.3f, 1);
    case LogLevel::Error:
    case LogLevel::Fatal: return ImVec4(1.0f, 0.45f, 0.45f, 1);
    default: return ImVec4(0.92f, 0.92f, 0.92f, 1);
    }
}

bool ContainsNoCase(const std::string& text, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != text.end();
}

} // namespace

void ConsolePanel::Submit(const std::string& line) {
    console_.Execute(line);
    input.clear();
    history_pos_ = -1;
    suggestions_.clear();
    last_tab_.clear();
    scroll_to_bottom_ = true;
}

void ConsolePanel::HistoryUp() {
    const auto& h = console_.History();
    if (h.empty()) return;
    history_pos_ = history_pos_ < 0 ? static_cast<int>(h.size()) - 1 : std::max(0, history_pos_ - 1);
    input = h[static_cast<usize>(history_pos_)];
}

void ConsolePanel::HistoryDown() {
    const auto& h = console_.History();
    if (history_pos_ < 0) return;
    if (++history_pos_ >= static_cast<int>(h.size())) {
        history_pos_ = -1;
        input.clear();
        return;
    }
    input = h[static_cast<usize>(history_pos_)];
}

void ConsolePanel::TabComplete() {
    const std::string completed = console_.CompleteLine(input);
    if (completed != input) {
        input = completed;
        suggestions_.clear();
        last_tab_.clear();
        return;
    }
    // Nothing more to add: a second Tab lists what it could be.
    const usize word = input.find_last_of(';') == std::string::npos ? 0 : input.find_last_of(';') + 1;
    std::string typed = input.substr(word);
    typed.erase(0, typed.find_first_not_of(" \t") == std::string::npos ? typed.size() : typed.find_first_not_of(" \t"));
    if (last_tab_ == input) suggestions_ = console_.Complete(typed);
    last_tab_ = input;
}

bool ConsolePanel::Passes(const ConsoleLine& line) const {
    switch (line.level) {
    case LogLevel::Trace:
        if (!show_trace) return false;
        break;
    case LogLevel::Info:
        if (!show_info) return false;
        break;
    case LogLevel::Warn:
        if (!show_warn) return false;
        break;
    case LogLevel::Error:
    case LogLevel::Fatal:
        if (!show_error) return false;
        break;
    }
    if (!category.empty() && line.category != category) return false;
    return ContainsNoCase(line.text, filter);
}

std::vector<ConsolePanel::Entry> ConsolePanel::VisibleLines() {
    console_.Pump();
    std::vector<Entry> entries;
    for (const ConsoleLine& line : console_.Output()) {
        if (!Passes(line)) continue;
        if (collapse_repeats && !entries.empty()) {
            const ConsoleLine& previous = *entries.back().line;
            if (previous.level == line.level && previous.category == line.category && previous.text == line.text) {
                ++entries.back().count;
                continue;
            }
        }
        entries.push_back({&line, 1});
    }
    return entries;
}

std::vector<std::string> ConsolePanel::Categories() {
    console_.Pump();
    std::vector<std::string> names;
    for (const ConsoleLine& line : console_.Output()) {
        if (line.category.empty()) continue;
        if (std::find(names.begin(), names.end(), line.category) == names.end()) names.push_back(line.category);
    }
    return names;
}

void ConsolePanel::Draw(bool playing) {
    ImGui::PushID(this);
    console_.Pump();
    if (playing && !last_playing_ && clear_on_play) { // each play session starts with a clean console
        console_.Clear();
        scroll_to_bottom_ = true;
    }
    last_playing_ = playing;
    char filter_buf[128];
    std::snprintf(filter_buf, sizeof(filter_buf), "%s", filter.c_str());
    ImGui::SetNextItemWidth(180);
    if (ImGui::InputTextWithHint("##filter", "Filter", filter_buf, sizeof(filter_buf))) filter = filter_buf;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    const std::vector<std::string> categories = Categories();
    if (ImGui::BeginCombo("##category", category.empty() ? "All categories" : category.c_str())) {
        if (ImGui::Selectable("All categories", category.empty())) category.clear();
        for (const std::string& name : categories) {
            ImGui::PushID(name.c_str());
            if (ImGui::Selectable(name.c_str(), category == name)) category = name;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) console_.Clear();
    ImGui::SameLine();
    bool capture = console_.CapturingLog();
    if (ImGui::Checkbox("Show log", &capture)) console_.CaptureLog(capture);
    ImGui::SameLine();
    ImGui::Checkbox("Cheats", &console_.allow_cheats);
    ImGui::SameLine();
    ImGui::Checkbox("Collapse", &collapse_repeats);
    ImGui::SameLine();
    ImGui::Checkbox("Clear on Play", &clear_on_play);
    ImGui::Checkbox("Trace", &show_trace);
    ImGui::SameLine();
    ImGui::Checkbox("Info", &show_info);
    ImGui::SameLine();
    ImGui::Checkbox("Warn", &show_warn);
    ImGui::SameLine();
    ImGui::Checkbox("Error", &show_error);

    const float footer = ImGui::GetFrameHeightWithSpacing() * (suggestions_.empty() ? 1.0f : 2.0f);
    ImGui::BeginChild("output", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    for (const Entry& entry : VisibleLines()) {
        ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(entry.line->level));
        ImGui::TextUnformatted(entry.line->text.c_str());
        ImGui::PopStyleColor();
        if (entry.count > 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu)", entry.count);
        }
    }
    if (scroll_to_bottom_ || ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    scroll_to_bottom_ = false;
    ImGui::EndChild();

    if (!suggestions_.empty()) {
        std::string list;
        for (usize i = 0; i < suggestions_.size() && i < 12; ++i) list += (i ? "  " : "") + suggestions_[i];
        if (suggestions_.size() > 12) list += "  ... (" + std::to_string(suggestions_.size()) + ")";
        ImGui::TextDisabled("%s", list.c_str());
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", input.c_str());
    ImGui::SetNextItemWidth(-1);
    const ImGuiInputTextFlags flags =
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion;
    auto callback = [](ImGuiInputTextCallbackData* data) -> int {
        auto* self = static_cast<ConsolePanel*>(data->UserData);
        self->input.assign(data->Buf, static_cast<usize>(data->BufTextLen));
        if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
            if (data->EventKey == ImGuiKey_UpArrow) self->HistoryUp();
            else if (data->EventKey == ImGuiKey_DownArrow) self->HistoryDown();
        } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
            self->TabComplete();
        }
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, self->input.c_str());
        return 0;
    };
    if (ImGui::InputTextWithHint("##input", "Command or variable (Tab completes)", buf, sizeof(buf), flags, callback, this)) {
        Submit(buf);
        ImGui::SetKeyboardFocusHere(-1);
    } else {
        input = buf;
    }
    ImGui::PopID();
}

void ConsolePanel::Overlay(bool toggle_pressed, float screen_width, float height_fraction) {
    if (toggle_pressed) open = !open;
    if (!open) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screen_width, io.DisplaySize.y * height_fraction));
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (ImGui::Begin("Console##overlay", &open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
        Draw();
    ImGui::End();
}

} // namespace aether::editor
