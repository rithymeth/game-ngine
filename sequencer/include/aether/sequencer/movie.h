#pragma once

#include "aether/ecs/world.h"
#include "aether/scene/entity_guid.h"
#include "aether/sequencer/player.h"
#include "aether/sequencer/sequence.h"

#include <functional>
#include <string>
#include <vector>

// Rendering a sequence to frames (Phase 27 step 6, docs/design/PHASE_SPECS.md
// §27.7): a fixed-step loop that steps a SequencePlayer one frame at a time
// and hands each frame to a sink. Frame i is at exactly i / fps seconds
// (computed from the integer, so nothing drifts), so the same sequence on the
// same world renders the same frames every time, whatever the machine's speed.
// What the sink does with a frame (draws it and writes an image, checks it in
// a test, feeds an encoder) is the caller's; this is the headless loop.

namespace aether::seq {

struct MovieOptions {
    f32 fps = 0.0f;         // 0: the sequence's own
    u32 start_frame = 0;    // the first frame rendered (events before it don't fire)
    u32 end_frame = 0;      // the last frame rendered; 0: the last frame of the sequence
    bool fire_events = true; // Event, Audio and Animation keys reach the player's hooks
};

// Called for each frame, after the tracks and the CineCameras were applied.
// Returning false stops the render.
using FrameSink = std::function<bool(u32 frame, f32 time, World& world)>;

struct MovieResult {
    u32 frames = 0;       // how many were handed to the sink
    u32 total_frames = 0; // how many the sequence has at this frame rate
    bool completed = false; // every requested frame was rendered (and the sink never stopped it)
    std::vector<std::string> problems; // bad options, and what the player couldn't bind
};

// The number of frames a sequence of `duration` seconds has at `fps`: one at
// every i / fps up to and including the end (24 fps over 2 s is 49 frames).
u32 MovieFrameCount(f32 duration, f32 fps);

// Renders the sequence on `world`. `setup` (optional) is called with the
// player before the first frame, to set its hooks (the Spawn track's spawner,
// on_event, on_audio ...). Camera Cut and Spawn effects are the player's, and
// are undone when the render ends.
MovieResult RenderMovie(const LevelSequence& sequence, World& world, const GuidIndex& guids, const MovieOptions& options,
                        const FrameSink& sink, const std::function<void(SequencePlayer&)>& setup = {});

} // namespace aether::seq
