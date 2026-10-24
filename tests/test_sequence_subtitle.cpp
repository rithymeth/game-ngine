#include "test_framework.h"

#include "aether/ecs/world.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/sequencer/player.h"
#include "aether/sequencer/sequence.h"
#include "aether/sequencer/sequence_system.h"

#include <map>
#include <string>

// M8.7: Subtitle tracks (dialogue lines shown for a range of the sequence),
// their file format and validation, the player's and system's view of them,
// and Skip/Stop hiding them.

using namespace aether;
using namespace aether::seq;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

SubtitleKey Line(f32 time, f32 duration, const char* text, const char* speaker = "") {
    SubtitleKey k;
    k.time = time;
    k.duration = duration;
    k.text = text;
    k.speaker = speaker;
    return k;
}

LevelSequence Dialogue() {
    LevelSequence s;
    s.name = "Dialogue";
    s.duration = 6.0f;
    Track t;
    t.id = "lines";
    t.type = TrackType::Subtitle;
    t.subtitles = {Line(0.5f, 1.5f, "dlg.one", "dlg.who"), Line(2.0f, 1.0f, "dlg.two"), Line(4.0f, 2.0f, "dlg.three")};
    s.tracks.push_back(t);
    return s;
}

const char* Shown(const SequencePlayer& p) { return p.Subtitle() ? p.Subtitle()->text.c_str() : ""; }

} // namespace

AETHER_TEST(SequenceSubtitle_JsonRoundTripAndValidation) {
    const LevelSequence s = Dialogue();
    CHECK(ValidateSequence(s).empty());
    LevelSequence back;
    std::string error;
    CHECK(SequenceFromJson(SequenceToJson(s), back, &error));
    CHECK(back.tracks.size() == 1 && back.tracks[0].type == TrackType::Subtitle && back.tracks[0].subtitles.size() == 3);
    CHECK(back.tracks[0].subtitles[0].text == "dlg.one" && back.tracks[0].subtitles[0].speaker == "dlg.who");
    CHECK(back.tracks[0].subtitles[2].time == 4.0f && back.tracks[0].subtitles[2].duration == 2.0f);
    CHECK(SequenceToJson(s)["tracks"][0]["type"] == "subtitle");

    LevelSequence bad = s;
    bad.tracks[0].subtitles[1].text.clear();
    bad.tracks[0].subtitles[2].duration = 0.0f;
    bool sq024 = false, sq025 = false;
    for (const std::string& d : ValidateSequence(bad)) {
        sq024 = sq024 || d.rfind("SQ024", 0) == 0;
        sq025 = sq025 || d.rfind("SQ025", 0) == 0;
    }
    CHECK(sq024 && sq025);
    LevelSequence wrong = s;
    wrong.tracks[0].type = TrackType::Event;
    bool sq013 = false;
    for (const std::string& d : ValidateSequence(wrong)) sq013 = sq013 || d.rfind("SQ013", 0) == 0;
    CHECK(sq013);
    nlohmann::json missing = SequenceToJson(s);
    missing["tracks"][0]["subtitles"][0].erase("duration");
    CHECK(!SequenceFromJson(missing, back, &error));
}

AETHER_TEST(SequenceSubtitle_FollowsThePlayheadAndSkipHidesIt) {
    World world;
    GuidIndex guids;
    const LevelSequence s = Dialogue();
    SequencePlayer p(s, world, guids);
    CHECK(p.Problems().empty());
    const auto at = [&](f32 t) {
        p.SetTime(t);
        p.Evaluate();
        return std::string(Shown(p));
    };
    CHECK(at(0.0f).empty());
    CHECK(at(0.5f) == "dlg.one"); // shown from its time ...
    CHECK(at(1.99f) == "dlg.one");
    CHECK(at(2.0f) == "dlg.two"); // ... to time + duration (exclusive)
    CHECK(at(3.5f).empty());
    CHECK(at(5.0f) == "dlg.three");
    CHECK(at(1.0f) == "dlg.one"); // scrubbing back is the same function of time
    // Played frame by frame at 1/144 s: each line appears on the first frame at or after its time.
    p.Stop();
    CHECK(p.Subtitle() == nullptr);
    p.Play();
    f32 shown_at = -1.0f;
    for (int frame = 0; frame < 144 && shown_at < 0.0f; ++frame) {
        p.Update(1.0f / 144.0f);
        if (p.Subtitle()) shown_at = p.Time();
    }
    CHECK(shown_at >= 0.5f && shown_at < 0.5f + 1.0f / 144.0f + 1e-5f);
    p.SetTime(4.5f);
    p.Play();
    p.Update(0.01f);
    CHECK(std::string(Shown(p)) == "dlg.three");
    p.Skip(); // the last line is still on at the end: Skip must take it off
    CHECK(p.Subtitle() == nullptr && !p.Playing() && p.Time() == p.Duration());
    // Muted tracks show nothing.
    LevelSequence muted = Dialogue();
    muted.tracks[0].mute = true;
    SequencePlayer q(muted, world, guids);
    q.SetTime(1.0f);
    q.Evaluate();
    CHECK(q.Subtitle() == nullptr);
}

AETHER_TEST(SequenceSubtitle_NestedSequenceLinesShowInTheParent) {
    World world;
    GuidIndex guids;
    std::map<std::string, LevelSequence> files;
    files["Child.asequence"] = Dialogue();
    LevelSequence parent;
    parent.duration = 10.0f;
    Track t;
    t.id = "subs";
    t.type = TrackType::Subsequence;
    SubKey k;
    k.time = 2.0f;
    k.duration = 6.0f;
    k.sequence = "Child.asequence";
    t.subs = {k};
    parent.tracks.push_back(t);
    SequencePlayer p(parent, world, guids, [&](const std::string& path) -> const LevelSequence* {
        const auto it = files.find(path);
        return it == files.end() ? nullptr : &it->second;
    });
    p.SetTime(2.6f);
    p.Evaluate();
    CHECK(std::string(Shown(p)) == "dlg.one");
    p.SetTime(9.0f);
    p.Evaluate();
    CHECK(p.Subtitle() == nullptr);
}

AETHER_TEST(SequenceSubtitle_SystemReportsTheLineAndClearsItOnSkip) {
    World world;
    GuidIndex guids;
    RegisterSequenceComponents();
    const LevelSequence s = Dialogue();
    SequenceSystem system(world, guids, [&](const std::string& path) -> const LevelSequence* { return path == "D.asequence" ? &s : nullptr; });
    SequenceComponent c;
    c.sequence = "D.asequence";
    c.auto_play = true;
    const Entity e = world.CreateEntity(c);
    system.Update(0.0f);
    for (int i = 0; i < 60; ++i) system.Update(1.0f / 60.0f); // t = 1.0
    Entity owner;
    const SubtitleKey* line = system.Subtitle(&owner);
    CHECK(line != nullptr && line->text == "dlg.one" && owner == e);
    CHECK(world.GetComponent<SequenceComponent>(e)->GetSubtitle() == "dlg.one");
    world.GetComponent<SequenceComponent>(e)->Skip();
    system.Update(1.0f / 60.0f);
    CHECK(system.Subtitle() == nullptr);
    CHECK(world.GetComponent<SequenceComponent>(e)->GetSubtitle().empty());
    bool skipped = false;
    for (const SequenceEvent& ev : system.Events()) skipped = skipped || (ev.kind == SequenceEvent::Kind::Finished && ev.skipped);
    CHECK(skipped);
}
