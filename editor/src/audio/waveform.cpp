#include "audio/waveform.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aether::editor {

std::vector<WaveformColumn> BuildPeaks(const audio::SoundWave& s, u32 columns, u32 channel, usize from, usize to) {
    std::vector<WaveformColumn> out;
    const usize frames = s.Frames();
    if (columns == 0 || frames == 0 || s.channels == 0) return out;
    channel = std::min(channel, s.channels - 1);
    if (to == 0 || to > frames) to = frames;
    if (from >= to) return out;
    out.resize(columns);
    const f64 per = static_cast<f64>(to - from) / columns;
    for (u32 c = 0; c < columns; ++c) {
        // Each column covers at least one frame, so zoomed in it shows the sample it lands on.
        const usize a = from + static_cast<usize>(c * per);
        const usize b = std::max(a + 1, std::min(to, from + static_cast<usize>((c + 1) * per)));
        f32 lo = 1e9f, hi = -1e9f;
        f64 sum = 0.0;
        for (usize i = a; i < b && i < to; ++i) {
            const f32 x = s.samples[i * s.channels + channel];
            lo = std::min(lo, x), hi = std::max(hi, x);
            sum += static_cast<f64>(x) * x;
        }
        out[c] = {lo, hi, static_cast<f32>(std::sqrt(sum / static_cast<f64>(b - a)))};
    }
    return out;
}

SoundStats AnalyzeSound(const audio::SoundWave& s) {
    SoundStats st;
    if (s.samples.empty()) return st;
    f64 sum = 0.0, sq = 0.0;
    for (f32 x : s.samples) {
        st.peak = std::max(st.peak, std::fabs(x));
        sum += x;
        sq += static_cast<f64>(x) * x;
        st.clipped += std::fabs(x) >= 1.0f ? 1 : 0;
    }
    const f64 n = static_cast<f64>(s.samples.size());
    st.peak_db = audio::GainToDb(st.peak);
    st.rms_db = audio::GainToDb(static_cast<f32>(std::sqrt(sq / n)));
    st.dc_offset = static_cast<f32>(sum / n);
    return st;
}

void WaveformView::SetSound(const audio::SoundWave* sound) {
    Stop();
    sound_ = sound;
    playhead_ = sel_from_ = sel_to_ = 0.0f;
    stats_ = sound != nullptr ? AnalyzeSound(*sound) : SoundStats{};
    ShowAll();
}

void WaveformView::ShowAll() {
    view_start_ = 0;
    view_frames_ = sound_ != nullptr ? sound_->Frames() : 0;
}

void WaveformView::Clamp() {
    if (sound_ == nullptr) return;
    const usize frames = sound_->Frames();
    view_frames_ = std::clamp<usize>(view_frames_, std::min<usize>(16, frames), frames);
    view_start_ = std::min(view_start_, frames - view_frames_);
}

void WaveformView::ZoomAt(f32 factor, f32 anchor) {
    if (sound_ == nullptr || factor <= 0.0f) return;
    anchor = std::clamp(anchor, 0.0f, 1.0f);
    const f64 pivot = view_start_ + anchor * static_cast<f64>(view_frames_);
    view_frames_ = static_cast<usize>(std::max(1.0, std::round(view_frames_ / static_cast<f64>(factor))));
    Clamp();
    view_start_ = static_cast<usize>(std::max(0.0, pivot - anchor * static_cast<f64>(view_frames_)));
    Clamp();
}

void WaveformView::ScrollBy(i64 frames) {
    if (sound_ == nullptr) return;
    view_start_ = static_cast<usize>(std::max<i64>(0, static_cast<i64>(view_start_) + frames));
    Clamp();
}

void WaveformView::SetPlayhead(f32 seconds) { playhead_ = sound_ == nullptr ? 0.0f : std::clamp(seconds, 0.0f, sound_->Duration()); }

void WaveformView::SetSelection(f32 from, f32 to) {
    const f32 d = sound_ != nullptr ? sound_->Duration() : 0.0f;
    sel_from_ = std::clamp(std::min(from, to), 0.0f, d);
    sel_to_ = std::clamp(std::max(from, to), 0.0f, d);
}

bool WaveformView::Play() {
    if (mixer_ == nullptr || sound_ == nullptr) return false;
    Stop();
    audio::PlayParams p;
    p.start_time = HasSelection() && (playhead_ < sel_from_ || playhead_ >= sel_to_) ? sel_from_ : playhead_;
    // Looping a selection: the mixer loops whole sounds, so the view wraps the voice itself (Tick).
    voice_ = mixer_->Play(sound_, p);
    return voice_ != 0;
}

void WaveformView::Stop() {
    if (mixer_ != nullptr && voice_ != 0) mixer_->Stop(voice_, 0.01f);
    voice_ = 0;
}

bool WaveformView::Playing() const { return mixer_ != nullptr && voice_ != 0 && mixer_->IsPlaying(voice_); }

void WaveformView::Tick() {
    if (voice_ == 0 || mixer_ == nullptr) return;
    if (!mixer_->IsPlaying(voice_)) {
        voice_ = 0;
        return;
    }
    playhead_ = std::max(0.0f, mixer_->PlaybackTime(voice_));
    if (HasSelection() && playhead_ >= sel_to_) {
        // Round again from the selection's start.
        playhead_ = sel_from_;
        Play();
    }
}

void WaveformView::Draw(f32 height) {
    Tick();
    ImGui::PushID(this);
    if (sound_ == nullptr) {
        ImGui::TextDisabled("No sound selected.");
        ImGui::PopID();
        return;
    }
    // Toolbar and facts.
    if (ImGui::Button(Playing() ? "Stop" : "Play")) Playing() ? Stop() : (void)Play();
    ImGui::SameLine();
    if (ImGui::Button("Fit")) ShowAll();
    ImGui::SameLine();
    ImGui::Text("%s  %u Hz  %s  %.3f s  peak %.1f dB  RMS %.1f dB%s", sound_->name.c_str(), sound_->sample_rate, sound_->channels == 1 ? "mono" : "stereo",
                sound_->Duration(), stats_.peak_db, stats_.rms_db, stats_.clipped > 0 ? "  CLIPPING" : "");

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const f32 width = std::max(ImGui::GetContentRegionAvail().x, 64.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(28, 30, 34, 255));
    const f32 rate = static_cast<f32>(sound_->sample_rate);
    auto x_of = [&](f32 seconds) { return origin.x + (seconds * rate - static_cast<f32>(view_start_)) / std::max<f32>(1.0f, static_cast<f32>(view_frames_)) * width; };
    if (HasSelection()) {
        draw->AddRectFilled(ImVec2(std::max(origin.x, x_of(sel_from_)), origin.y), ImVec2(std::min(origin.x + width, x_of(sel_to_)), origin.y + height),
                            IM_COL32(80, 120, 200, 60));
    }
    const u32 lanes = sound_->channels;
    const f32 lane_h = height / static_cast<f32>(lanes);
    for (u32 c = 0; c < lanes; ++c) {
        const f32 mid = origin.y + lane_h * (static_cast<f32>(c) + 0.5f);
        draw->AddLine(ImVec2(origin.x, mid), ImVec2(origin.x + width, mid), IM_COL32(70, 70, 80, 255));
        const auto peaks = BuildPeaks(*sound_, static_cast<u32>(width), c, view_start_, view_start_ + view_frames_);
        for (usize i = 0; i < peaks.size(); ++i) {
            const f32 x = origin.x + static_cast<f32>(i) + 0.5f;
            draw->AddLine(ImVec2(x, mid - peaks[i].max * lane_h * 0.48f), ImVec2(x, mid - peaks[i].min * lane_h * 0.48f + 1.0f), IM_COL32(110, 200, 140, 255));
            draw->AddLine(ImVec2(x, mid - peaks[i].rms * lane_h * 0.48f), ImVec2(x, mid + peaks[i].rms * lane_h * 0.48f), IM_COL32(170, 240, 190, 255));
        }
    }
    draw->AddLine(ImVec2(x_of(playhead_), origin.y), ImVec2(x_of(playhead_), origin.y + height), IM_COL32(240, 90, 80, 255), 2.0f);
    // Mouse: click sets the playhead, drag selects, wheel zooms about the cursor, Shift+wheel scrolls.
    ImGui::InvisibleButton("##wave", ImVec2(width, height));
    const ImGuiIO& io = ImGui::GetIO();
    const f32 at = static_cast<f32>(view_start_ + (io.MousePos.x - origin.x) / width * static_cast<f32>(view_frames_)) / rate;
    if (ImGui::IsItemActivated()) {
        SetPlayhead(at);
        sel_from_ = sel_to_ = playhead_;
        dragging_selection_ = true;
    }
    if (ImGui::IsItemActive() && dragging_selection_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) SetSelection(playhead_, at);
    if (!ImGui::IsItemActive()) dragging_selection_ = false;
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
        if (io.KeyShift) ScrollBy(static_cast<i64>(-io.MouseWheel * static_cast<f32>(view_frames_) * 0.1f));
        else ZoomAt(io.MouseWheel > 0.0f ? 1.25f : 0.8f, (io.MousePos.x - origin.x) / width);
    }
    // Time ruler.
    char label[32];
    std::snprintf(label, sizeof(label), "%.3f s", static_cast<f64>(view_start_) / rate);
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(std::max(0.0f, width - 60.0f));
    std::snprintf(label, sizeof(label), "%.3f s", static_cast<f64>(view_start_ + view_frames_) / rate);
    ImGui::TextDisabled("%s", label);
    ImGui::PopID();
}

} // namespace aether::editor
