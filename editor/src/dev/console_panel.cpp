#include "dev/console_panel.h"

#include "aether/core/cvars.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace aether::editor {

void ConsolePanel::Log(const std::string& text, bool error) {
    scrollback_.push_back((error ? "[error] " : "") + text);
    if (scrollback_.size() > 500u) {
        scrollback_.pop_front();
    }
}

std::string ConsolePanel::Draw() {
    if (!open_) {
        return {};
    }
    std::string submitted;
    if (ImGui::Begin("Console###ConsolePanel", &open_)) {
        // Scrollback.
        const float footer_h = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("##console_scroll", ImVec2(0, -footer_h), true);
        for (const std::string& line : scrollback_) {
            const bool is_error = line.rfind("[error] ", 0) == 0;
            if (is_error) {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", line.c_str() + 8);
            } else {
                ImGui::TextUnformatted(line.c_str());
            }
        }
        if (autoscroll_) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        char input_buf[512] = {};
        std::memcpy(input_buf, input_.c_str(), std::min<usize>(input_.size(), sizeof(input_buf) - 1));
        if (ImGui::InputText("##console_input", input_buf, sizeof(input_buf),
                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            const std::string cmd = input_buf;
            submitted = cmd;
            if (history_.empty() || history_.back() != cmd) {
                history_.push_back(cmd);
                if (history_.size() > 50u) {
                    history_.pop_front();
                }
            }
            history_index_ = history_.size();
            input_.clear();
            autoscroll_ = true;
        }

        // ArrowUp/ArrowDown navigate the command history.
        if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !history_.empty()) {
            if (history_index_ > 0) {
                --history_index_;
            }
            input_ = history_[history_index_];
        }
        if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !history_.empty()) {
            if (history_index_ < history_.size() - 1) {
                ++history_index_;
                input_ = history_[history_index_];
            } else {
                input_.clear();
            }
        }

        // Tab completes the current first token against CVar/command names.
        if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Tab) && !input_.empty()) {
            const usize space = input_.find(' ');
            const std::string token = (space == std::string::npos) ? input_ : input_.substr(0, space);
            std::string match;
            for (const cvars::CVar* c : cvars::All()) {
                if (c->name.rfind(token, 0) == 0 && c->name != token) {
                    if (match.empty()) match = c->name;
                    else match.clear(); // ambiguous
                }
            }
            for (const cvars::ConsoleCommand* c : cvars::Commands()) {
                if (c->name.rfind(token, 0) == 0 && c->name != token) {
                    if (match.empty()) match = c->name;
                    else match.clear();
                }
            }
            if (!match.empty()) {
                input_ = match + " ";
            }
        }
    }
    ImGui::End();
    return submitted;
}

} // namespace aether::editor
