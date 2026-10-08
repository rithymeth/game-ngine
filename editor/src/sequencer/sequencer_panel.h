#pragma once

#include "sequencer/sequence_document.h"

#include "aether/ecs/world.h"
#include "aether/scene/entity_guid.h"
#include "aether/sequencer/player.h"

#include <memory>
#include <optional>
#include <vector>

namespace aether::editor {

// The Sequencer editor (Phase 27 step 5, §27.6): a toolbar (undo, add a
// track, transport, time), a timeline (ruler, a row per track and value
// channel with its keys as diamonds, a playhead; click the ruler to scrub,
// click a key to select it, drag it to move it), and an inspector for the
// selected key, the track and the diagnostics.
//
// It previews on a World and GuidIndex the host gives it (a demo scene, or
// the open level): scrubbing and playing apply the tracks to that world, so
// the Viewport shows the result. Spawn tracks make empty entities there and
// remove them again; audio and animation keys are ignored in the preview.
class SequencerPanel {
public:
    SequencerPanel(SequenceDocument& document, World& world, GuidIndex& guids);
    ~SequencerPanel();
    SequencerPanel(const SequencerPanel&) = delete;
    SequencerPanel& operator=(const SequencerPanel&) = delete;

    void Draw();

    // The preview player, made again whenever the document changes.
    seq::SequencePlayer& Player();
    void Scrub(f32 time); // sets the playhead and applies the tracks there
    void Play();
    void Pause();
    void Stop();
    void Skip();
    bool Playing() { return Player().Playing(); }
    f32 Time() { return Player().Time(); }
    // Advances the preview by `dt` when playing (Draw does it with the frame time).
    void Update(f32 dt);

    void SelectKey(const KeyRef& key) { selected_ = key; }
    void ClearSelection() { selected_.reset(); }
    const std::optional<KeyRef>& Selected() const { return selected_; }

    f32 pixels_per_second = 120.0f;
    bool loop = false;

private:
    void Rebuild();
    void DrawToolbar();
    void DrawTimeline();
    void DrawInspector();
    // The entities of the preview world that have a GUID, for the binding picker.
    std::vector<EntityGuid> BindableEntities() const;
    std::string NameOf(const EntityGuid& guid) const;

    SequenceDocument& doc_;
    World& world_;
    GuidIndex& guids_;
    std::unique_ptr<seq::SequencePlayer> player_;
    u64 built_revision_ = 0;
    f32 time_before_rebuild_ = 0.0f;
    std::optional<KeyRef> selected_;
    bool dragging_key_ = false;
    bool scrubbing_ = false;
    usize selected_track_ = 0;
};

} // namespace aether::editor
