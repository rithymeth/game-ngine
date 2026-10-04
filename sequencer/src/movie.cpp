#include "aether/sequencer/movie.h"

#include "aether/scene/gameplay.h"

#include <algorithm>
#include <cmath>

namespace aether::seq {

u32 MovieFrameCount(f32 duration, f32 fps) {
    if (!(fps > 0.0f) || !(duration >= 0.0f)) return 0;
    return static_cast<u32>(std::floor(duration * fps + 1e-3f)) + 1;
}

MovieResult RenderMovie(const LevelSequence& sequence, World& world, const GuidIndex& guids, const MovieOptions& options,
                        const FrameSink& sink, const std::function<void(SequencePlayer&)>& setup) {
    MovieResult result;
    const f32 fps = options.fps != 0.0f ? options.fps : sequence.fps; // 0 asks for the sequence's own
    if (!(fps > 0.0f)) {
        result.problems.push_back("movie: the frame rate must be positive");
        return result;
    }
    if (sequence.tracks.empty()) {
        result.problems.push_back("movie: the sequence has no tracks");
        return result;
    }
    SequencePlayer player(sequence, world, guids);
    result.total_frames = MovieFrameCount(player.Duration(), fps);
    const u32 last = options.end_frame == 0 ? result.total_frames - 1 : std::min(options.end_frame, result.total_frames - 1);
    if (options.start_frame > last) {
        result.problems.push_back("movie: the start frame is past the last frame");
        return result;
    }
    if (setup) setup(player);
    if (!options.fire_events) {
        player.on_event = nullptr;
        player.on_audio = nullptr;
        player.on_animation = nullptr;
    }
    result.completed = true;
    for (u32 frame = options.start_frame; frame <= last; ++frame) {
        const f32 time = static_cast<f32>(frame) / fps;
        if (frame == options.start_frame && frame > 0) {
            player.SetTime(time); // events before the range are not fired
            player.Evaluate();
        } else {
            player.AdvanceTo(time);
        }
        ApplyCineCameras(world);
        ++result.frames;
        if (sink && !sink(frame, time, world)) {
            result.completed = frame == last;
            break;
        }
    }
    result.problems.insert(result.problems.end(), player.Problems().begin(), player.Problems().end());
    return result;
}

} // namespace aether::seq
