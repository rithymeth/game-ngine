#pragma once

#include "aether/audio/mixer.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace aether::audio {

// Sound cues (Phase 17 step 3, docs/design/PHASE_SPECS.md §17.3): a small
// graph that decides what plays each time the cue is triggered: which
// variation, at what volume and pitch, in what order and for how long.
// Saved as `.acue` JSON. Evaluating a cue gives a plan of sounds with
// start offsets; the CuePlayer (cue_player.h) schedules them on the mixer.

enum class CueNodeType : u8 {
    Wave,         // plays a sound; no children
    Random,       // one child, by weight (optionally never the same twice in a row)
    Sequence,     // one child, in turn, each time the cue plays
    Modulator,    // randomises volume and pitch for its child
    Concatenator, // children one after another
    Loop,         // its child `count` times, or forever (0)
    Mix,          // all children at once, each with its own volume
    Delay,        // waits a random time, then its child
};
const char* CueNodeTypeName(CueNodeType type);
bool ParseCueNodeType(const std::string& name, CueNodeType& out);

struct CueNode {
    u32 id = 0;
    CueNodeType type = CueNodeType::Wave;
    std::vector<u32> children;
    f32 x = 0.0f, y = 0.0f; // editor position
    // Wave
    std::string sound;
    bool looping = false;
    // Random: weights per child (missing ones are 1).
    std::vector<f32> weights;
    bool no_repeat = false;
    // Modulator
    f32 volume_min_db = 0.0f, volume_max_db = 0.0f;
    f32 pitch_min = 1.0f, pitch_max = 1.0f;
    // Loop: 0 is forever.
    u32 count = 0;
    // Mix: volume per child in dB (missing ones are 0).
    std::vector<f32> input_db;
    // Delay, in seconds.
    f32 delay_min = 0.0f, delay_max = 0.0f;
};

struct SoundCue {
    std::string name;
    std::vector<CueNode> nodes;
    u32 root = 0; // the node the output plays
    // Output settings for everything the cue plays.
    f32 volume_db = 0.0f;
    f32 pitch = 1.0f;
    std::string bus = "SFX"; // falls back to Master when the mixer has no such bus
    u8 priority = 128;
    VirtualMode virtual_mode = VirtualMode::Continue;
    bool spatial = false;
    f32 spatial_blend = 1.0f;
    AttenuationSettings attenuation{};
    f32 doppler = 1.0f;
    bool occlusion = false;

    const CueNode* Find(u32 id) const;
    CueNode* Find(u32 id);
    u32 NextId() const; // one more than the largest id
};

struct CueDiagnostic {
    std::string code; // CU001...
    std::string message;
    u32 node = 0;
    bool error = true; // false: a warning
};
// `sound_exists` checks Wave sounds when given.
std::vector<CueDiagnostic> ValidateCue(const SoundCue& cue, const std::function<bool(const std::string&)>& sound_exists = {});

nlohmann::json CueToJson(const SoundCue& cue);
bool CueFromJson(const nlohmann::json& j, SoundCue& out, std::string* error = nullptr);
std::string SaveCue(const SoundCue& cue);
bool LoadCue(const std::string& text, SoundCue& out, std::string* error = nullptr);

// --- Evaluation -------------------------------------------------------------------------------

// A sound to start `offset` seconds after the cue does.
struct CueItem {
    std::string sound;
    f64 offset = 0.0;
    f32 volume_db = 0.0f;
    f32 pitch = 1.0f;
    bool loop = false;
    u32 node = 0; // the Wave node, for the editor
};
// A loop that goes on forever: evaluate `node` again at `offset`, and every time it ends.
struct CueTail {
    u32 node = 0;
    f64 offset = 0.0;
    f32 volume_db = 0.0f;
    f32 pitch = 1.0f;
};
struct CuePlan {
    std::vector<CueItem> items;
    std::vector<CueTail> tails;
    f64 duration = 0.0; // seconds; infinite when something loops forever
};

// The choices a cue remembers between plays (Random's last pick, Sequence's
// next), and the random numbers. One per cue asset.
class CueState {
public:
    explicit CueState(u64 seed = 0x9E3779B97F4A7C15ull) : rng_(seed == 0 ? 1 : seed) {}
    f32 Uniform(f32 min, f32 max); // in [min, max]
    std::map<u32, u32> last_pick, next_in_sequence;

private:
    u64 rng_;
};

// A sound's length in seconds, or < 0 when it's unknown.
using SoundDuration = std::function<f64(const std::string&)>;

CuePlan EvaluateCue(const SoundCue& cue, CueState& state, const SoundDuration& duration);
// One node at an offset, with the volume and pitch above it (for tails).
CuePlan EvaluateCueNode(const SoundCue& cue, u32 node, CueState& state, const SoundDuration& duration, f64 offset = 0.0, f32 volume_db = 0.0f,
                        f32 pitch = 1.0f);

} // namespace aether::audio
