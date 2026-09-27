#include "audio/mixer_panel.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aether::editor {

using audio::AudioEffect;
using audio::BusId;

void MeterBallistics::Update(f32 input, f32 dt) {
    const f32 db = audio::GainToDb(input);
    // Rise at once, fall at a steady rate.
    level_db = db >= level_db ? db : std::max(db, level_db - fall_db_per_second * dt);
    if (db >= peak_db) {
        peak_db = db;
        held_for = 0.0f;
    } else {
        held_for += dt;
        if (held_for > hold_seconds) peak_db = std::max(level_db, peak_db - fall_db_per_second * dt);
    }
}

std::vector<EffectParam> EffectParams(const AudioEffect& e) {
    const char* kind = e.Name();
    if (std::strcmp(kind, "Filter") == 0) return {{"Frequency", 20.0f, 20000.0f, "%.0f Hz"}, {"Q", 0.1f, 20.0f, "%.2f"}, {"Gain", -24.0f, 24.0f, "%.1f dB"}};
    if (std::strcmp(kind, "Compressor") == 0) {
        return {{"Threshold", -60.0f, 0.0f, "%.1f dB"}, {"Ratio", 1.0f, 20.0f, "%.1f:1"}, {"Attack", 0.1f, 200.0f, "%.1f ms"},
                {"Release", 1.0f, 2000.0f, "%.0f ms"},  {"Makeup", 0.0f, 24.0f, "%.1f dB"}};
    }
    if (std::strcmp(kind, "Reverb") == 0) {
        return {{"Room size", 0.0f, 1.0f, "%.2f"}, {"Damping", 0.0f, 1.0f, "%.2f"}, {"Wet", 0.0f, 1.0f, "%.2f"}, {"Dry", 0.0f, 1.0f, "%.2f"},
                {"Width", 0.0f, 1.0f, "%.2f"}};
    }
    return {};
}

std::vector<f32> ReadEffect(const AudioEffect& e) {
    if (const auto* f = dynamic_cast<const audio::FilterEffect*>(&e)) return {f->Filter().frequency, f->Filter().q, f->Filter().gain_db};
    if (const auto* c = dynamic_cast<const audio::CompressorEffect*>(&e)) return {c->threshold_db, c->ratio, c->attack_ms, c->release_ms, c->makeup_db};
    if (const auto* r = dynamic_cast<const audio::ReverbEffect*>(&e)) return {r->room_size, r->damping, r->wet, r->dry, r->width};
    return {};
}

void WriteEffect(AudioEffect& e, const std::vector<f32>& v) {
    if (auto* f = dynamic_cast<audio::FilterEffect*>(&e); f != nullptr && v.size() >= 3) {
        f->Set(v[0], v[1], v[2]);
    } else if (auto* c = dynamic_cast<audio::CompressorEffect*>(&e); c != nullptr && v.size() >= 5) {
        c->threshold_db = v[0], c->ratio = v[1], c->attack_ms = v[2], c->release_ms = v[3], c->makeup_db = v[4];
    } else if (auto* r = dynamic_cast<audio::ReverbEffect*>(&e); r != nullptr && v.size() >= 5) {
        r->room_size = v[0], r->damping = v[1], r->wet = v[2], r->dry = v[3], r->width = v[4];
    }
}

MixerPanel::EffectView& MixerPanel::ViewOf(AudioEffect* effect) {
    auto& slot = effects_[effect];
    if (!slot) slot = std::make_shared<EffectView>();
    EffectView& v = *slot;
    if (!v.requested) {
        v.requested = true;
        if (!mixer_.Threaded()) {
            v.values = ReadEffect(*effect);
            v.bypass = effect->bypass;
            v.ready = true;
        } else {
            // Ask the audio thread, which owns the effect, for its settings.
            mixer_.Post([effect, view = slot](audio::Mixer&) {
                view->values = ReadEffect(*effect);
                view->bypass = effect->bypass;
                view->ready.store(true, std::memory_order_release);
            });
        }
    }
    return v;
}

bool MixerPanel::EffectValues(BusId bus, usize index, std::vector<f32>& values, bool& bypass) {
    AudioEffect* e = mixer_.EffectAt(bus, index);
    if (e == nullptr) return false;
    EffectView& v = ViewOf(e);
    if (!v.ready.load(std::memory_order_acquire)) return false;
    values = v.values;
    bypass = v.bypass_override.value_or(v.bypass);
    return true;
}

void MixerPanel::SetBypass(BusId bus, usize index, bool bypass) {
    AudioEffect* e = mixer_.EffectAt(bus, index);
    if (e == nullptr) return;
    EffectView& v = ViewOf(e);
    if (v.ready.load(std::memory_order_acquire)) {
        v.bypass = bypass;
        v.bypass_override.reset();
    } else {
        v.bypass_override = bypass;
    }
    mixer_.Post([e, bypass](audio::Mixer&) { e->bypass = bypass; });
}

void MixerPanel::SetEffectParam(BusId bus, usize index, usize param, f32 value) {
    AudioEffect* e = mixer_.EffectAt(bus, index);
    if (e == nullptr) return;
    EffectView& v = ViewOf(e);
    if (!v.ready.load(std::memory_order_acquire) || param >= v.values.size()) return; // settings not known yet
    v.values[param] = value;
    mixer_.Post([e, values = v.values](audio::Mixer&) { WriteEffect(*e, values); });
}

void MixerPanel::UpdateMeters(f32 dt) {
    for (BusId b = 0; b < mixer_.BusCount(); ++b) {
        const audio::BusMeter m = mixer_.Meter(b);
        for (u32 c = 0; c < 2; ++c) meters_[{b, c}].Update(m.peak[c], dt);
    }
}

void MixerPanel::Draw() {
    ImGui::PushID(this);
    UpdateMeters(ImGui::GetIO().DeltaTime);
    ImGui::Text("%zu voices (%zu real, %zu virtual)%s", mixer_.VoiceCount(), mixer_.RealVoiceCount(), mixer_.VirtualVoiceCount(),
                mixer_.Threaded() ? "  - live" : "");
    auto flags = [&](Tab t) { return show_tab_ == t ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None; };
    if (ImGui::BeginTabBar("##mixer_tabs")) {
        if (ImGui::BeginTabItem("Buses", nullptr, flags(Tab::Buses))) {
            for (BusId b = 0; b < mixer_.BusCount(); ++b) {
                if (b > 0) ImGui::SameLine();
                DrawStrip(b);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Effects", nullptr, flags(Tab::Effects))) {
            DrawEffects();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Voices", nullptr, flags(Tab::Voices))) {
            DrawVoices();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    show_tab_.reset();
    ImGui::PopID();
}

void MixerPanel::DrawStrip(BusId bus) {
    ImGui::PushID(static_cast<int>(bus));
    ImGui::BeginGroup();
    const std::string& name = mixer_.BusName(bus);
    ImGui::TextUnformatted(name.c_str());
    // Meters: two bars (L, R) from -60 to +6 dB, RMS darker inside the level, and the held peak as a line.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const f32 height = 160.0f, bar = 8.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    auto y_of = [&](f32 db) { return origin.y + height * (1.0f - std::clamp((db + 60.0f) / 66.0f, 0.0f, 1.0f)); };
    const audio::BusMeter live = mixer_.Meter(bus);
    for (u32 c = 0; c < 2; ++c) {
        const MeterBallistics& m = meters_[{bus, c}];
        const f32 x = origin.x + static_cast<f32>(c) * (bar + 2.0f);
        draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + bar, origin.y + height), IM_COL32(25, 25, 28, 255));
        const u32 color = m.level_db > -0.1f ? IM_COL32(230, 70, 60, 255) : m.level_db > -6.0f ? IM_COL32(230, 200, 60, 255) : IM_COL32(80, 200, 110, 255);
        draw->AddRectFilled(ImVec2(x, y_of(m.level_db)), ImVec2(x + bar, origin.y + height), color);
        draw->AddRectFilled(ImVec2(x, y_of(audio::GainToDb(live.rms[c]))), ImVec2(x + bar, origin.y + height), IM_COL32(40, 120, 70, 255));
        draw->AddLine(ImVec2(x, y_of(m.peak_db)), ImVec2(x + bar, y_of(m.peak_db)), IM_COL32(255, 255, 255, 255), 2.0f);
    }
    draw->AddLine(ImVec2(origin.x, y_of(0.0f)), ImVec2(origin.x + 2 * bar + 2.0f, y_of(0.0f)), IM_COL32(200, 200, 200, 120));
    ImGui::Dummy(ImVec2(2 * bar + 4.0f, height));
    ImGui::SameLine();
    f32 volume = mixer_.BusVolume(bus);
    if (ImGui::VSliderFloat("##fader", ImVec2(28, height), &volume, -60.0f, 12.0f, "%.0f")) SetFader(bus, volume);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) SetFader(bus, 0.0f); // back to unity
    const bool muted = mixer_.BusMuted(bus);
    if (muted) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1));
    if (ImGui::Button("M", ImVec2(24, 0))) ToggleMute(bus);
    if (muted) ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu fx", mixer_.EffectCount(bus));
    ImGui::EndGroup();
    ImGui::PopID();
}

void MixerPanel::DrawEffects() {
    bool any = false;
    for (BusId b = 0; b < mixer_.BusCount(); ++b) {
        for (usize i = 0; i < mixer_.EffectCount(b); ++i) {
            any = true;
            AudioEffect* e = mixer_.EffectAt(b, i);
            ImGui::PushID(e);
            ImGui::SeparatorText((mixer_.BusName(b) + " / " + e->Name()).c_str());
            std::vector<f32> values;
            bool bypass = false;
            if (!EffectValues(b, i, values, bypass)) {
                ImGui::TextDisabled("Waiting for the audio thread...");
                ImGui::PopID();
                continue;
            }
            if (ImGui::Checkbox("Bypass", &bypass)) SetBypass(b, i, bypass);
            const std::vector<EffectParam> params = EffectParams(*e);
            for (usize p = 0; p < params.size() && p < values.size(); ++p) {
                f32 v = values[p];
                if (ImGui::SliderFloat(params[p].name, &v, params[p].min, params[p].max, params[p].format)) SetEffectParam(b, i, p, v);
            }
            if (!mixer_.Threaded()) {
                if (const auto* c = dynamic_cast<const audio::CompressorEffect*>(e)) {
                    ImGui::ProgressBar(std::clamp(c->GainReductionDb() / 24.0f, 0.0f, 1.0f), ImVec2(-1, 0), "gain reduction");
                }
            }
            ImGui::PopID();
        }
    }
    if (!any) ImGui::TextDisabled("No effects on any bus.");
}

void MixerPanel::DrawVoices() {
    const auto voices = mixer_.Voices();
    if (voices.empty()) {
        ImGui::TextDisabled("Nothing playing.");
        return;
    }
    if (!ImGui::BeginTable("##voices", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) return;
    for (const char* h : {"Voice", "Time", "State", "Distance", "Gain", "Pan", "Pitch"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (const auto& v : voices) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%u", v.id);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f s", v.time);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(v.info.is_virtual ? "virtual" : "real");
        ImGui::TableNextColumn();
        ImGui::Text("%.1f m", v.info.distance);
        ImGui::TableNextColumn();
        ImGui::Text("%.1f dB", audio::GainToDb(v.info.audibility));
        ImGui::TableNextColumn();
        ImGui::Text("%+.2f", v.info.pan);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", v.info.pitch);
    }
    ImGui::EndTable();
}

} // namespace aether::editor
