#include "test_framework.h"
#include "sequencer/sequence_document.h"
#include "sequencer/sequencer_panel.h"
#include "workspace/editor_workspace.h"

#include "aether/ecs/world.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/gameplay.h"

#include <imgui.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>

// The sequencer editor (Phase 27 step 5, §27.6): the document's edits, undo
// and diagnostics, and the panel previewing on a world.

using namespace aether;
using namespace aether::editor;
using namespace aether::seq;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigMacOSXBehaviors = false;
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400, 900);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1300, 800));
        ImGui::Begin("Test");
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

bool Near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

struct Level {
    World world;
    GuidIndex guids;
    Entity actor;
    EntityGuid guid;
    Level() {
        (void)GetComponentId<Transform>();
        (void)GetComponentId<Tags>();
        actor = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
        guid = EnsureGuid(world, actor, &guids);
    }
};

} // namespace

AETHER_TEST(SequenceEditor_TracksUndoAndRedo) {
    SequenceDocument doc;
    CHECK(!doc.dirty() && !doc.CanUndo());
    const usize move = doc.AddTrack(TrackType::Transform, "Move");
    const usize events = doc.AddTrack(TrackType::Event, "Markers");
    CHECK(move == 0 && events == 1 && doc.Sequence().tracks.size() == 2 && doc.dirty());
    CHECK(doc.Sequence().tracks[0].channels.size() == 3 && doc.Sequence().tracks[1].channels.empty());
    CHECK(doc.Sequence().tracks[0].id != doc.Sequence().tracks[1].id);
    CHECK(doc.RenameTrack(0, "Slide") && doc.Sequence().tracks[0].name == "Slide");
    CHECK(doc.SetMute(0, true) && doc.Sequence().tracks[0].mute);
    CHECK(doc.Undo() && !doc.Sequence().tracks[0].mute);
    CHECK(doc.Undo() && doc.Sequence().tracks[0].name == "Move");
    CHECK(doc.Redo() && doc.Sequence().tracks[0].name == "Slide");
    CHECK(doc.RemoveTrack(1) && doc.Sequence().tracks.size() == 1);
    CHECK(doc.Undo() && doc.Sequence().tracks.size() == 2);
    CHECK(!doc.RemoveTrack(9) && !doc.RenameTrack(9, "x"));
    // A new edit clears the redo history.
    CHECK(doc.CanRedo());
    doc.SetName("Intro");
    CHECK(!doc.CanRedo());
}

AETHER_TEST(SequenceEditor_KeysStaySortedAndEditable) {
    SequenceDocument doc;
    const usize t = doc.AddTrack(TrackType::Transform, "Move");
    CHECK(doc.AddKey(t, 0, 2.0f, 20.0f) == 0);
    CHECK(doc.AddKey(t, 0, 0.0f, 0.0f) == 0); // sorted in front
    CHECK(doc.AddKey(t, 0, 1.0f, 10.0f) == 1);
    const auto& keys = doc.Sequence().tracks[t].channels[0].keys;
    CHECK(keys.size() == 3 && keys[0].time == 0.0f && keys[1].time == 1.0f && keys[2].time == 2.0f);
    CHECK(doc.AddKey(t, 0, 1.0f, 99.0f) == 1 && keys.size() == 3 && keys[1].value == 99.0f); // same time: replaces
    CHECK(doc.AddKey(t, 7, 1.0f, 1.0f) == -1 && doc.AddKey(t, 0, -1.0f, 1.0f) == -1);

    const KeyRef mid{t, KeyRef::Lane::Channel, 0, 1};
    CHECK(doc.SetKeyValue(mid, 5.0f) && keys[1].value == 5.0f);
    CHECK(doc.SetKeyInterp(mid, Interp::Bezier) && keys[1].interp == Interp::Bezier);
    // Moving past a neighbour re-sorts and returns the new index; onto another key is refused.
    CHECK(doc.MoveKey(mid, 3.0f) == 2 && keys[2].time == 3.0f && keys[2].value == 5.0f);
    CHECK(doc.MoveKey(KeyRef{t, KeyRef::Lane::Channel, 0, 0}, 2.0f) == -1);
    CHECK(doc.RemoveKey(KeyRef{t, KeyRef::Lane::Channel, 0, 2}) && keys.size() == 2);
    CHECK(!doc.RemoveKey(KeyRef{t, KeyRef::Lane::Channel, 0, 9}));

    // Rotation and event keys.
    CHECK(doc.AddRotationKey(t, 1.0f, Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.0f)) == 0);
    CHECK(doc.AddRotationKey(t, 0.5f, Quaternion::Identity()) == 0 && doc.Sequence().tracks[t].rotation.size() == 2);
    const usize e = doc.AddTrack(TrackType::Event, "Markers");
    CHECK(doc.AddEvent(e, 2.0f, "B") >= 0 && doc.AddEvent(e, 1.0f, "A", "x") == 0);
    CHECK(doc.Sequence().tracks[e].events[0].name == "A" && doc.Sequence().tracks[e].events[1].name == "B");
    CHECK(doc.SetEvent(KeyRef{e, KeyRef::Lane::Event, 0, 1}, "C", "p") && doc.Sequence().tracks[e].events[1].name == "C");
    CHECK(doc.AddEvent(t, 1.0f, "no") == -1); // not an Event track
    CHECK(doc.KeyCount(t, KeyRef::Lane::Rotation) == 2 && Near(doc.KeyTime(KeyRef{e, KeyRef::Lane::Event, 0, 0}), 1.0f));
}

AETHER_TEST(SequenceEditor_ADragIsOneUndoStep) {
    SequenceDocument doc;
    const usize t = doc.AddTrack(TrackType::Transform, "Move");
    doc.AddKey(t, 0, 1.0f, 1.0f);
    KeyRef key{t, KeyRef::Lane::Channel, 0, 0};
    doc.BeginEdit();
    for (f32 time : {1.1f, 1.4f, 1.9f, 2.5f}) {
        const i32 i = doc.MoveKey(key, time);
        CHECK(i == 0);
    }
    doc.EndEdit();
    CHECK(Near(doc.KeyTime(key), 2.5f));
    CHECK(doc.Undo() && Near(doc.KeyTime(key), 1.0f)); // all four moves undone together
    CHECK(doc.Redo() && Near(doc.KeyTime(key), 2.5f));
}

AETHER_TEST(SequenceEditor_LockedTracksRefuseEdits) {
    SequenceDocument doc;
    const usize t = doc.AddTrack(TrackType::Transform, "Move");
    doc.AddKey(t, 0, 1.0f, 1.0f);
    CHECK(doc.SetLocked(t, true));
    const u64 revision = doc.Revision();
    CHECK(doc.AddKey(t, 0, 2.0f, 2.0f) == -1 && !doc.RemoveKey(KeyRef{t, KeyRef::Lane::Channel, 0, 0}));
    CHECK(doc.MoveKey(KeyRef{t, KeyRef::Lane::Channel, 0, 0}, 2.0f) == -1 && !doc.RemoveTrack(t) && !doc.RenameTrack(t, "x"));
    CHECK(!doc.SetBinding(t, {}) && doc.Revision() == revision);
    CHECK(doc.SetLocked(t, false) && doc.AddKey(t, 0, 2.0f, 2.0f) >= 0); // unlocking works on a locked track
}

AETHER_TEST(SequenceEditor_DiagnosticsFollowTheEdits) {
    Level level;
    SequenceDocument doc;
    CHECK(!doc.Problems().empty()); // no tracks: SQ001
    const usize t = doc.AddTrack(TrackType::Transform, "Move", level.guid);
    doc.AddKey(t, 0, 0.0f, 0.0f);
    CHECK(doc.Problems().empty());
    doc.SetBinding(t, {});
    CHECK(doc.Problems().size() == 1 && doc.Problems()[0].rfind("SQ008", 0) == 0);
    const usize p = doc.AddTrack(TrackType::Property, "Field", level.guid);
    CHECK(doc.SetPropertyTarget(p, "Camera", "fov_degrees") && !doc.SetPropertyTarget(t, "Camera", "x"));
    CHECK(doc.SetDuration(5.0f) && doc.Sequence().duration == 5.0f && !doc.SetDuration(-1.0f));
    CHECK(doc.SetFps(30.0f) && !doc.SetFps(0.0f));
    doc.AddKey(p, 0, 1.0f, 1.0f);
    doc.AddKey(p, 0, 9.0f, 1.0f); // past the duration
    bool sq003 = false;
    for (const std::string& m : doc.Problems()) sq003 = sq003 || m.rfind("SQ003", 0) == 0;
    CHECK(sq003);
}

AETHER_TEST(SequenceEditor_SaveLoadAndDirty) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_sequence_editor";
    std::filesystem::remove_all(dir);
    Level level;
    SequenceDocument doc;
    const usize t = doc.AddTrack(TrackType::Transform, "Move", level.guid);
    doc.AddKey(t, 0, 0.0f, 0.0f);
    doc.AddKey(t, 0, 2.0f, 8.0f);
    doc.SetName("Intro");
    CHECK(doc.dirty());
    std::string error;
    CHECK(doc.Save(dir / "Sequences/intro.asequence", &error) && !doc.dirty());

    SequenceDocument loaded;
    loaded.AddTrack(TrackType::Event, "stale");
    CHECK(loaded.Load(dir / "Sequences/intro.asequence", &error));
    CHECK(loaded.Sequence().name == "Intro" && loaded.Sequence().tracks.size() == 1 && loaded.Sequence().tracks[0].channels[0].keys.size() == 2);
    CHECK(!loaded.dirty() && !loaded.CanUndo()); // the history starts fresh

    // A failed load leaves the document alone.
    std::ofstream(dir / "bad.asequence") << "{ not json";
    const u64 revision = loaded.Revision();
    CHECK(!loaded.Load(dir / "bad.asequence", &error) && !error.empty());
    CHECK(!loaded.Load(dir / "missing.asequence", &error));
    CHECK(loaded.Sequence().name == "Intro" && loaded.Revision() == revision);
}

AETHER_TEST(SequenceEditor_PanelScrubsAndPlaysThePreview) {
    HeadlessImGui ui;
    Level level;
    SequenceDocument doc;
    const usize t = doc.AddTrack(TrackType::Transform, "Move", level.guid);
    doc.AddKey(t, 0, 0.0f, 0.0f);
    doc.AddKey(t, 0, 2.0f, 10.0f);
    SequencerPanel panel(doc, level.world, level.guids);
    CHECK(Near(panel.Player().Duration(), 2.0f));
    panel.Scrub(1.0f);
    CHECK(Near(level.world.GetComponent<Transform>(level.actor)->position.x, 5.0f));
    panel.Scrub(0.5f);
    panel.Scrub(1.5f);
    panel.Scrub(1.0f);
    CHECK(Near(level.world.GetComponent<Transform>(level.actor)->position.x, 5.0f)); // back and forth: the same result
    panel.Scrub(0.0f);
    panel.Play();
    CHECK(panel.Playing());
    panel.Update(0.5f);
    CHECK(Near(panel.Time(), 0.5f) && Near(level.world.GetComponent<Transform>(level.actor)->position.x, 2.5f));
    panel.Pause();
    panel.Update(1.0f);
    CHECK(Near(panel.Time(), 0.5f));
    panel.Stop();
    CHECK(Near(panel.Time(), 0.0f) && Near(level.world.GetComponent<Transform>(level.actor)->position.x, 0.0f));

    // Editing the document makes the preview follow: the player is rebuilt.
    doc.AddKey(t, 0, 2.0f, 20.0f);
    panel.Scrub(1.0f);
    CHECK(Near(level.world.GetComponent<Transform>(level.actor)->position.x, 10.0f));
    doc.Undo();
    panel.Scrub(1.0f);
    CHECK(Near(level.world.GetComponent<Transform>(level.actor)->position.x, 5.0f));
    // Loop: playing past the end wraps.
    panel.loop = true;
    panel.Scrub(0.0f);
    panel.Play();
    panel.Update(2.5f);
    CHECK(panel.Playing() && Near(panel.Time(), 0.5f));
}

AETHER_TEST(SequenceEditor_PreviewSpawnsAreRemovedWithThePanel) {
    Level level;
    SequenceDocument doc;
    const usize t = doc.AddTrack(TrackType::Spawn, "Props");
    (void)t;
    LevelSequence s = doc.Sequence();
    SpawnKey key;
    key.time = 0.0f;
    key.prefab = "Crate";
    s.tracks[0].spawns = {key};
    s.duration = 2.0f;
    SequenceDocument spawning(s);
    usize before = 0;
    level.world.ForEach<Transform>([&](Transform&) { ++before; });
    {
        SequencerPanel panel(spawning, level.world, level.guids);
        panel.Scrub(1.0f);
        usize during = 0;
        level.world.ForEach<Transform>([&](Transform&) { ++during; });
        CHECK(during == before + 1);
        panel.Scrub(0.0f); // still inside [0, end): still alive
        spawning.SetName("edit"); // a rebuild removes the old preview entity before making its own
        panel.Scrub(1.0f);
        usize again = 0;
        level.world.ForEach<Transform>([&](Transform&) { ++again; });
        CHECK(again == before + 1);
    }
    usize after = 0;
    level.world.ForEach<Transform>([&](Transform&) { ++after; });
    CHECK(after == before);
}

AETHER_TEST(SequenceEditor_PanelDrawsAndSelectsKeysHeadless) {
    HeadlessImGui ui;
    Level level;
    SequenceDocument doc;
    const usize t = doc.AddTrack(TrackType::Transform, "Move", level.guid);
    doc.AddKey(t, 0, 0.0f, 0.0f);
    doc.AddKey(t, 0, 2.0f, 10.0f);
    const usize e = doc.AddTrack(TrackType::Event, "Markers");
    doc.AddEvent(e, 1.0f, "Boom");
    doc.AddTrack(TrackType::Visibility, "Light", level.guid);
    SequencerPanel panel(doc, level.world, level.guids);
    panel.SelectKey(KeyRef{t, KeyRef::Lane::Channel, 0, 1});
    for (int frame = 0; frame < 3; ++frame) ui.Frame([&] { panel.Draw(); });
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    panel.SelectKey(KeyRef{e, KeyRef::Lane::Event, 0, 0});
    ui.Frame([&] { panel.Draw(); });
    CHECK(panel.Selected() && panel.Selected()->lane == KeyRef::Lane::Event);
    panel.ClearSelection();
    CHECK(!panel.Selected());
}

AETHER_TEST(SequenceEditor_WorkspaceShowsTheSampleSequence) {
    HeadlessImGui ui;
    EditorWorkspace ws;
    const i64 tool = ws.FindTool("Sequencer");
    CHECK(tool >= 0 && std::string(ws.ToolCategory(static_cast<usize>(tool))) == "Cinematics");
    ws.Select(static_cast<usize>(tool));
    for (int frame = 0; frame < 2; ++frame) ui.Frame([&] { ws.DrawHubContents(); });
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
}
