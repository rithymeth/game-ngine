#include "sequencer_panel.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"

#include <algorithm>
#include <cmath>

namespace aether::editor {

using namespace seq;

namespace {

constexpr f32 kRowHeight = 22.0f;
constexpr f32 kLabelWidth = 200.0f;
constexpr f32 kRulerHeight = 24.0f;

const char* kTrackTypeNames[] = {"Transform", "Property", "Event", "Visibility", "Spawn", "Camera Cut", "Audio", "Animation", "Fade", "Subsequence"};
const char* kInterpNames[] = {"Constant", "Linear", "Bezier"};

struct Lane {
    KeyRef::Lane lane;
    usize channel;
    std::string label;
};

// The rows a track draws: one per value channel, plus one for each key list it has.
std::vector<Lane> LanesOf(const Track& t) {
    std::vector<Lane> lanes;
    for (usize c = 0; c < t.channels.size(); ++c) {
        lanes.push_back({KeyRef::Lane::Channel, c, t.channels[c].name.empty() ? "value" : t.channels[c].name});
    }
    if (t.type == TrackType::Transform) lanes.push_back({KeyRef::Lane::Rotation, 0, "rotation"});
    if (t.type == TrackType::Event) lanes.push_back({KeyRef::Lane::Event, 0, "events"});
    if (t.type == TrackType::Spawn) lanes.push_back({KeyRef::Lane::Spawn, 0, "spawns"});
    if (t.type == TrackType::CameraCut) lanes.push_back({KeyRef::Lane::Cut, 0, "cuts"});
    if (t.type == TrackType::Audio) lanes.push_back({KeyRef::Lane::Audio, 0, "audio"});
    if (t.type == TrackType::Animation) lanes.push_back({KeyRef::Lane::Animation, 0, "animation"});
    if (t.type == TrackType::Subsequence) lanes.push_back({KeyRef::Lane::Sub, 0, "subsequences"});
    return lanes;
}

} // namespace

SequencerPanel::SequencerPanel(SequenceDocument& document, World& world, GuidIndex& guids)
    : doc_(document), world_(world), guids_(guids) {}

SequencerPanel::~SequencerPanel() {
    player_.reset(); // despawns anything the preview spawned
}

void SequencerPanel::Rebuild() {
    f32 time = 0.0f;
    bool playing = false;
    if (player_) {
        time = player_->Time();
        playing = player_->Playing();
    }
    player_.reset(); // its destructor removes spawned previews before the new one makes its own
    player_ = std::make_unique<SequencePlayer>(doc_.Sequence(), world_, guids_);
    player_->on_spawn = [this](const Track&, Entity, const SpawnKey& key) {
        return world_.CreateEntity(Transform{key.position, key.rotation});
    };
    player_->on_despawn = [this](Entity e) {
        if (world_.IsAlive(e)) world_.DestroyEntity(e);
    };
    player_->loop = loop;
    player_->SetTime(time);
    if (playing) player_->Play();
    built_revision_ = doc_.Revision();
}

SequencePlayer& SequencerPanel::Player() {
    if (!player_ || built_revision_ != doc_.Revision()) Rebuild();
    return *player_;
}

void SequencerPanel::Scrub(f32 time) {
    SequencePlayer& p = Player();
    p.SetTime(time);
    p.Evaluate();
}

void SequencerPanel::Play() {
    SequencePlayer& p = Player();
    if (p.Time() >= p.Duration()) p.SetTime(0.0f);
    p.loop = loop;
    p.Play();
}

void SequencerPanel::Pause() { Player().Pause(); }

void SequencerPanel::Stop() {
    SequencePlayer& p = Player();
    p.Stop();
    p.Evaluate();
}

void SequencerPanel::Skip() { Player().Skip(); }

void SequencerPanel::Update(f32 dt) {
    SequencePlayer& p = Player();
    p.loop = loop;
    p.Update(dt);
}

std::vector<EntityGuid> SequencerPanel::BindableEntities() const {
    std::vector<EntityGuid> out;
    world_.ForEach<IdComponent>([&](IdComponent& id) { out.push_back(id.guid); });
    return out;
}

std::string SequencerPanel::NameOf(const EntityGuid& guid) const {
    if (guid.IsNull()) return "(none)";
    const Entity e = guids_.Find(world_, guid);
    if (e.IsNull()) return "(missing) " + ToString(guid).substr(0, 8);
    if (const Tags* tags = world_.GetComponent<Tags>(e); tags && !tags->names.empty()) return tags->names.front();
    return "Entity " + std::to_string(e.index);
}

void SequencerPanel::Draw() {
    Update(ImGui::GetIO().DeltaTime);
    DrawToolbar();
    ImGui::Separator();
    if (ImGui::BeginChild("##timeline", ImVec2(0, -150), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) DrawTimeline();
    ImGui::EndChild();
    DrawInspector();
}

void SequencerPanel::DrawToolbar() {
    ImGui::BeginDisabled(!doc_.CanUndo());
    if (ImGui::Button("Undo")) doc_.Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanRedo());
    if (ImGui::Button("Redo")) doc_.Redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Add Track")) ImGui::OpenPopup("##addtrack");
    if (ImGui::BeginPopup("##addtrack")) {
        for (int t = 0; t < 10; ++t) {
            if (ImGui::MenuItem(kTrackTypeNames[t])) {
                selected_track_ = doc_.AddTrack(static_cast<TrackType>(t), kTrackTypeNames[t]);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    const bool playing = Player().Playing();
    if (ImGui::Button(playing ? "Pause" : "Play")) {
        if (playing) Pause();
        else Play();
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) Stop();
    ImGui::SameLine();
    if (ImGui::Button("Skip")) Skip();
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &loop);
    ImGui::SameLine();
    f32 time = Player().Time();
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::DragFloat("##time", &time, 0.01f, 0.0f, Player().Duration(), "%.2f s")) Scrub(time);
    ImGui::SameLine();
    ImGui::Text("/ %.2f s  %.0f fps", Player().Duration(), doc_.Sequence().fps);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::SliderFloat("Zoom", &pixels_per_second, 20.0f, 400.0f, "%.0f px/s");
    const std::vector<std::string> problems = doc_.Problems();
    if (!problems.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "%zu problem(s)", problems.size());
    }
}

void SequencerPanel::DrawTimeline() {
    const LevelSequence& s = doc_.Sequence();
    const f32 duration = std::max(Player().Duration(), 1.0f);
    const f32 width = kLabelWidth + duration * pixels_per_second + 40.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const f32 x0 = origin.x + kLabelWidth;
    const auto time_x = [&](f32 t) { return x0 + t * pixels_per_second; };

    // Ruler: a tick every second (and every frame when zoomed in).
    ImGui::InvisibleButton("##ruler", ImVec2(width, kRulerHeight));
    if (ImGui::IsItemActivated()) scrubbing_ = true;
    if (scrubbing_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const f32 t = (ImGui::GetIO().MousePos.x - x0) / pixels_per_second;
        Scrub(std::clamp(std::round(t * s.fps) / s.fps, 0.0f, Player().Duration()));
    } else {
        scrubbing_ = false;
    }
    const ImU32 line = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    for (int sec = 0; sec <= static_cast<int>(std::ceil(duration)); ++sec) {
        const f32 x = time_x(static_cast<f32>(sec));
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + kRulerHeight), line);
        char label[16];
        std::snprintf(label, sizeof label, "%d", sec);
        draw->AddText(ImVec2(x + 3, origin.y + 4), text, label);
    }

    // Rows.
    f32 y = origin.y + kRulerHeight + 4.0f;
    bool key_clicked = false;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (usize ti = 0; ti < s.tracks.size(); ++ti) {
        const Track& t = s.tracks[ti];
        // Track header row: the name, mute and lock toggles.
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        ImGui::PushID(static_cast<int>(ti));
        const bool header_selected = selected_track_ == ti;
        if (ImGui::Selectable((std::string(kTrackTypeNames[static_cast<int>(t.type)]) + ": " + (t.name.empty() ? t.id : t.name)).c_str(), header_selected,
                              0, ImVec2(kLabelWidth - 52.0f, kRowHeight - 2.0f))) {
            selected_track_ = ti;
        }
        ImGui::SameLine();
        bool mute = t.mute;
        if (ImGui::Checkbox("##m", &mute)) doc_.SetMute(ti, mute);
        ImGui::SameLine();
        bool lock = t.locked;
        if (ImGui::Checkbox("##l", &lock)) doc_.SetLocked(ti, lock);
        ImGui::PopID();
        y += kRowHeight;
        for (const Lane& lane : LanesOf(t)) {
            draw->AddText(ImVec2(origin.x + 16, y + 3), ImGui::GetColorU32(ImGuiCol_TextDisabled), lane.label.c_str());
            draw->AddLine(ImVec2(x0, y + kRowHeight - 1), ImVec2(x0 + duration * pixels_per_second, y + kRowHeight - 1), line);
            const usize count = doc_.KeyCount(ti, lane.lane, lane.channel);
            for (usize ki = 0; ki < count; ++ki) {
                const KeyRef ref{ti, lane.lane, lane.channel, ki};
                const f32 x = time_x(doc_.KeyTime(ref));
                const f32 cy = y + kRowHeight * 0.5f;
                const bool is_selected = selected_ && *selected_ == ref;
                const ImU32 color = is_selected ? IM_COL32(255, 200, 60, 255) : (t.locked ? IM_COL32(120, 120, 120, 255) : IM_COL32(120, 190, 255, 255));
                draw->AddQuadFilled(ImVec2(x, cy - 6), ImVec2(x + 6, cy), ImVec2(x, cy + 6), ImVec2(x - 6, cy), color);
                if (!key_clicked && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && std::fabs(mouse.x - x) <= 7.0f && std::fabs(mouse.y - cy) <= 8.0f) {
                    selected_ = ref;
                    dragging_key_ = true;
                    key_clicked = true;
                    doc_.BeginEdit();
                }
            }
            y += kRowHeight;
        }
        y += 4.0f;
    }
    // Dragging the selected key moves it (one undo step).
    if (dragging_key_ && selected_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const f32 t = std::max(0.0f, std::round(((mouse.x - x0) / pixels_per_second) * s.fps) / s.fps);
            if (std::fabs(t - doc_.KeyTime(*selected_)) > 1e-4f) {
                const i32 moved = doc_.MoveKey(*selected_, t);
                if (moved >= 0) selected_->index = static_cast<usize>(moved);
            }
        } else {
            dragging_key_ = false;
            doc_.EndEdit();
        }
    }
    // The playhead.
    const f32 px = time_x(Player().Time());
    draw->AddLine(ImVec2(px, origin.y), ImVec2(px, y), IM_COL32(255, 80, 80, 255), 2.0f);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 8.0f));
    ImGui::Dummy(ImVec2(width, 1.0f));
}

void SequencerPanel::DrawInspector() {
    const LevelSequence& s = doc_.Sequence();
    if (ImGui::BeginChild("##inspector", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        if (selected_track_ < s.tracks.size()) {
            const Track& t = s.tracks[selected_track_];
            std::string name = t.name;
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::InputText("Track", &name) && name != t.name) doc_.RenameTrack(selected_track_, name);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::BeginCombo("Entity", NameOf(t.binding).c_str())) {
                if (ImGui::Selectable("(none)", t.binding.IsNull())) doc_.SetBinding(selected_track_, {});
                for (const EntityGuid& g : BindableEntities()) {
                    if (ImGui::Selectable(NameOf(g).c_str(), g == t.binding)) doc_.SetBinding(selected_track_, g);
                }
                ImGui::EndCombo();
            }
            if (t.type == TrackType::Property) {
                std::string component = t.component, field = t.field;
                ImGui::SetNextItemWidth(160.0f);
                const bool a = ImGui::InputText("Component", &component);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160.0f);
                const bool b = ImGui::InputText("Field", &field);
                if (a || b) doc_.SetPropertyTarget(selected_track_, component, field);
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove Track")) {
                if (doc_.RemoveTrack(selected_track_)) {
                    selected_.reset();
                    selected_track_ = 0;
                }
            }
        }
        if (selected_ && selected_->track < s.tracks.size() && selected_->index < doc_.KeyCount(selected_->track, selected_->lane, selected_->channel)) {
            const Track& t = s.tracks[selected_->track];
            ImGui::Separator();
            f32 time = doc_.KeyTime(*selected_);
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::DragFloat("Time", &time, 0.01f, 0.0f, 3600.0f, "%.3f")) {
                const i32 moved = doc_.MoveKey(*selected_, time);
                if (moved >= 0) selected_->index = static_cast<usize>(moved);
            }
            if (selected_->lane == KeyRef::Lane::Channel) {
                const Key& k = t.channels[selected_->channel].keys[selected_->index];
                f32 value = k.value;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                if (ImGui::DragFloat("Value", &value, 0.05f)) doc_.SetKeyValue(*selected_, value);
                int interp = static_cast<int>(k.interp);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100.0f);
                if (ImGui::Combo("Interp", &interp, kInterpNames, 3)) doc_.SetKeyInterp(*selected_, static_cast<Interp>(interp));
            } else if (selected_->lane == KeyRef::Lane::Event) {
                const EventKey& e = t.events[selected_->index];
                std::string name = e.name, payload = e.payload;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f);
                const bool a = ImGui::InputText("Name", &name);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f);
                const bool b = ImGui::InputText("Payload", &payload);
                if (a || b) doc_.SetEvent(*selected_, name, payload);
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete Key")) {
                doc_.RemoveKey(*selected_);
                selected_.reset();
            }
        }
        const std::vector<std::string> problems = doc_.Problems();
        for (const std::string& p : problems) ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "%s", p.c_str());
    }
    ImGui::EndChild();
}

} // namespace aether::editor
