#include "anim/anim_viewer.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace aether::editor {

void ClipViewer::Sample() { anim::SampleClip(clip_, skeleton_, time_, loop, pose_); }

void ClipViewer::SetTime(f32 time) {
    time_ = std::clamp(time, 0.0f, clip_.duration);
    Sample();
}

void ClipViewer::Advance(f32 dt) {
    fired_.clear();
    if (playing && clip_.duration > 0.0f) {
        const f32 from = time_;
        f32 to = time_ + dt * speed;
        if (loop) {
            to = anim::ClipTime(clip_, to, true);
        } else if (to >= clip_.duration) {
            to = clip_.duration;
            playing = false;
        }
        std::vector<anim::NotifyPoint> points;
        anim::CollectNotifies(clip_, from, to, loop, points);
        for (const anim::NotifyPoint& p : points) {
            if (!p.end) fired_.push_back(p.notify->name);
        }
        time_ = to;
    }
    Sample();
}

void ClipViewer::Record() {
    undo_.push_back(clip_.notifies);
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    redo_.clear();
    dirty_ = true;
}

usize ClipViewer::AddNotify(const std::string& name, f32 time, f32 duration) {
    Record();
    clip_.notifies.push_back({name, std::clamp(time, 0.0f, clip_.duration), std::max(duration, 0.0f)});
    return clip_.notifies.size() - 1;
}

bool ClipViewer::MoveNotify(usize index, f32 time) {
    if (index >= clip_.notifies.size()) return false;
    Record();
    clip_.notifies[index].time = std::clamp(time, 0.0f, clip_.duration);
    return true;
}

bool ClipViewer::RenameNotify(usize index, const std::string& name) {
    if (index >= clip_.notifies.size() || name.empty()) return false;
    Record();
    clip_.notifies[index].name = name;
    return true;
}

bool ClipViewer::RemoveNotify(usize index) {
    if (index >= clip_.notifies.size()) return false;
    Record();
    clip_.notifies.erase(clip_.notifies.begin() + static_cast<std::ptrdiff_t>(index));
    if (selected_notify >= static_cast<i32>(clip_.notifies.size())) selected_notify = -1;
    return true;
}

bool ClipViewer::Undo() {
    if (undo_.empty()) return false;
    redo_.push_back(clip_.notifies);
    clip_.notifies = std::move(undo_.back());
    undo_.pop_back();
    dirty_ = true;
    return true;
}

bool ClipViewer::Redo() {
    if (redo_.empty()) return false;
    undo_.push_back(clip_.notifies);
    clip_.notifies = std::move(redo_.back());
    redo_.pop_back();
    dirty_ = true;
    return true;
}

std::vector<f32> ClipViewer::Curve(i32 bone, u32 component, u32 samples) const {
    std::vector<f32> out;
    if (bone < 0 || static_cast<usize>(bone) >= skeleton_.bones.size() || samples < 2) return out;
    anim::Pose p;
    for (u32 i = 0; i < samples; ++i) {
        anim::SampleClip(clip_, skeleton_, clip_.duration * static_cast<f32>(i) / static_cast<f32>(samples - 1), false, p);
        const Vec3& t = p.local[static_cast<usize>(bone)].translation;
        out.push_back(component == 0 ? t.x : component == 1 ? t.y : t.z);
    }
    return out;
}

void ClipViewer::DrawBones(i32 bone) {
    const anim::Bone& b = skeleton_.bones[static_cast<usize>(bone)];
    bool has_children = false;
    for (const anim::Bone& c : skeleton_.bones) has_children |= c.parent == bone;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
    if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf;
    if (bone == selected_bone) flags |= ImGuiTreeNodeFlags_Selected;
    const bool open = ImGui::TreeNodeEx((b.name + "##bone" + std::to_string(bone)).c_str(), flags);
    if (ImGui::IsItemClicked()) selected_bone = bone;
    if (open) {
        for (usize c = 0; c < skeleton_.bones.size(); ++c) {
            if (skeleton_.bones[c].parent == bone) DrawBones(static_cast<i32>(c));
        }
        ImGui::TreePop();
    }
}

void ClipViewer::Draw() {
    ImGui::PushID(this);
    // Playback bar.
    if (ImGui::Button(playing ? "Pause" : "Play")) playing = !playing;
    ImGui::SameLine();
    if (ImGui::Button("|<")) SetTime(0.0f);
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::DragFloat("Speed", &speed, 0.01f, 0.0f, 4.0f);
    ImGui::SameLine();
    f32 t = time_;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##time", &t, 0.0f, std::max(clip_.duration, 1e-3f), "%.3f s")) SetTime(t);
    Advance(ImGui::GetIO().DeltaTime);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##bones", ImVec2(220, avail.y), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Skeleton");
    for (usize b = 0; b < skeleton_.bones.size(); ++b) {
        if (skeleton_.bones[b].parent < 0) DrawBones(static_cast<i32>(b));
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##tracks", ImVec2(0, avail.y), ImGuiChildFlags_Borders);
    // Notify track: markers at their times; drag to move, right-click to add.
    ImGui::SeparatorText("Notifies");
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const f32 width = std::max(ImGui::GetContentRegionAvail().x, 50.0f), height = 28.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(40, 40, 45, 255));
    const f32 duration = std::max(clip_.duration, 1e-3f);
    auto x_of = [&](f32 time) { return origin.x + time / duration * width; };
    draw->AddLine(ImVec2(x_of(time_), origin.y), ImVec2(x_of(time_), origin.y + height), IM_COL32(230, 80, 80, 255), 2.0f);
    for (usize i = 0; i < clip_.notifies.size(); ++i) {
        const anim::AnimNotify& n = clip_.notifies[i];
        const u32 color = static_cast<i32>(i) == selected_notify ? IM_COL32(255, 200, 60, 255) : IM_COL32(120, 180, 255, 255);
        if (n.duration > 0.0f) draw->AddRectFilled(ImVec2(x_of(n.time), origin.y + 6), ImVec2(x_of(n.time + n.duration), origin.y + height - 6), color & 0x80FFFFFF);
        draw->AddTriangleFilled(ImVec2(x_of(n.time) - 5, origin.y + 2), ImVec2(x_of(n.time) + 5, origin.y + 2), ImVec2(x_of(n.time), origin.y + 12), color);
        draw->AddText(ImVec2(x_of(n.time) + 6, origin.y + 12), IM_COL32(220, 220, 220, 255), n.name.c_str());
    }
    ImGui::InvisibleButton("##notify_track", ImVec2(width, height), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const f32 mouse_time = std::clamp((ImGui::GetIO().MousePos.x - origin.x) / width * duration, 0.0f, clip_.duration);
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        dragging_notify_ = -1;
        for (usize i = 0; i < clip_.notifies.size(); ++i) {
            if (std::abs(x_of(clip_.notifies[i].time) - ImGui::GetIO().MousePos.x) < 6.0f) dragging_notify_ = static_cast<i32>(i);
        }
        selected_notify = dragging_notify_;
        if (dragging_notify_ >= 0) Record(); // one undo step for the whole drag
        else SetTime(mouse_time);            // scrub
    }
    if (ImGui::IsItemActive() && dragging_notify_ >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        clip_.notifies[static_cast<usize>(dragging_notify_)].time = mouse_time;
    }
    if (!ImGui::IsItemActive()) dragging_notify_ = -1;
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) selected_notify = static_cast<i32>(AddNotify("Notify", mouse_time));
    if (selected_notify >= 0 && static_cast<usize>(selected_notify) < clip_.notifies.size()) {
        const usize i = static_cast<usize>(selected_notify);
        std::string name = clip_.notifies[i].name;
        if (ImGui::InputText("Name", &name, ImGuiInputTextFlags_EnterReturnsTrue)) RenameNotify(i, name);
        f32 window = clip_.notifies[i].duration;
        if (ImGui::DragFloat("Duration", &window, 0.01f, 0.0f, clip_.duration)) {
            Record();
            clip_.notifies[i].duration = window;
        }
        if (ImGui::Button("Delete Notify")) RemoveNotify(i);
    }
    // Curves: the selected bone's translation over the clip.
    ImGui::SeparatorText("Curves");
    if (selected_bone >= 0) {
        static const char* kAxes[] = {"X", "Y", "Z"};
        for (u32 c = 0; c < 3; ++c) {
            const std::vector<f32> curve = Curve(selected_bone, c);
            ImGui::PlotLines(kAxes[c], curve.data(), static_cast<int>(curve.size()), 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(-1, 40));
        }
    } else {
        ImGui::TextDisabled("Select a bone to see its curves.");
    }
    ImGui::EndChild();
    ImGui::PopID();
}

} // namespace aether::editor
