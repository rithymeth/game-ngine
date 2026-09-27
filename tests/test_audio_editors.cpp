#include "audio/cue_editor.h"
#include "audio/mixer_panel.h"
#include "audio/waveform.h"
#include "test_framework.h"

#include "aether/audio/backend.h"

#include <imgui.h>

#include <cmath>
#include <filesystem>
#include <functional>
#include <thread>

using namespace aether;
using namespace aether::audio;
using namespace aether::editor;

// Phase 17 step 6: the waveform preview, the sound cue editor and the mixer
// panel, drawn headless.

namespace {

constexpr u32 kRate = 48000;

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
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
        ImGui::SetNextWindowSize(ImVec2(1400, 900));
        ImGui::Begin("Audio", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

SoundWave Dc(f32 level, usize frames) {
    SoundWave s;
    s.sample_rate = kRate;
    s.samples.assign(frames, level);
    return s;
}

void Render(Mixer& m, u32 frames) {
    std::vector<f32> out(static_cast<usize>(frames) * 2);
    for (u32 at = 0; at < frames; at += 256) m.Render(out.data() + at * 2, std::min(256u, frames - at));
}

} // namespace

AETHER_TEST(AudioEditor_WaveformPeaksStatsAndPreview) {
    SoundWave tone = GenerateTone(100.0f, 1.0f, kRate, 0.5f); // 100 Hz: 480 frames a cycle
    tone.name = "Tone";
    // Wide columns see whole cycles; zoomed in, a column is one sample.
    const auto peaks = BuildPeaks(tone, 50);
    AETHER_CHECK(peaks.size() == 50);
    for (const WaveformColumn& c : peaks) AETHER_CHECK(std::fabs(c.max - 0.5f) < 1e-3f && std::fabs(c.min + 0.5f) < 1e-3f && std::fabs(c.rms - 0.3536f) < 2e-3f);
    const auto zoomed = BuildPeaks(tone, 10, 0, 0, 10);
    AETHER_CHECK(zoomed.size() == 10 && zoomed[3].min == zoomed[3].max && zoomed[3].max == tone.samples[3]);
    const SoundWave dc = Dc(0.25f, 1000);
    AETHER_CHECK(BuildPeaks(dc, 4)[2].min == 0.25f && BuildPeaks(dc, 4)[2].max == 0.25f);
    SoundWave stereo = GenerateTone(100.0f, 0.1f, kRate, 0.5f, 2);
    for (usize i = 0; i < stereo.Frames(); ++i) stereo.samples[i * 2 + 1] = 0.0f; // right silent
    AETHER_CHECK(BuildPeaks(stereo, 4, 1)[0].max == 0.0f && BuildPeaks(stereo, 4, 0)[0].max > 0.4f);
    AETHER_CHECK(BuildPeaks(SoundWave{}, 10).empty() && BuildPeaks(tone, 0).empty() && BuildPeaks(tone, 4, 0, 100, 50).empty());

    const SoundStats st = AnalyzeSound(tone);
    AETHER_CHECK(std::fabs(st.peak - 0.5f) < 1e-3f && std::fabs(st.peak_db + 6.02f) < 0.02f && std::fabs(st.rms_db + 9.03f) < 0.05f);
    AETHER_CHECK(std::fabs(st.dc_offset) < 1e-3f && st.clipped == 0);
    SoundWave hot = Dc(0.5f, 10);
    hot.samples[3] = 1.0f, hot.samples[7] = -1.2f;
    AETHER_CHECK(AnalyzeSound(hot).clipped == 2 && AnalyzeSound(hot).dc_offset > 0.3f);

    // The view: zoom about a point, scroll within the sound, playhead and selection.
    WaveformView view;
    view.SetSound(&tone);
    AETHER_CHECK(view.ViewStart() == 0 && view.ViewFrames() == kRate);
    view.ZoomAt(4.0f, 0.5f);
    AETHER_CHECK(view.ViewFrames() == kRate / 4 && view.ViewStart() == kRate / 2 - kRate / 8); // the middle stays put
    view.ScrollBy(-1000000);
    AETHER_CHECK(view.ViewStart() == 0);
    view.ScrollBy(1000000);
    AETHER_CHECK(view.ViewStart() + view.ViewFrames() == kRate);
    view.ZoomAt(1e9f);
    AETHER_CHECK(view.ViewFrames() == 16); // never fewer than 16 frames
    view.ShowAll();
    view.SetPlayhead(5.0f);
    AETHER_CHECK(view.Playhead() == 1.0f);
    view.SetSelection(0.6f, 0.2f);
    AETHER_CHECK(view.HasSelection() && view.SelectionFrom() == 0.2f && view.SelectionTo() == 0.6f);

    // Preview from the playhead; with a selection, it loops the selection.
    Mixer mixer(kRate);
    view.SetMixer(&mixer);
    view.SetSelection(0.0f, 0.0f);
    view.SetPlayhead(0.25f);
    AETHER_CHECK(view.Play() && view.Playing() && std::fabs(mixer.PlaybackTime(mixer.Voices()[0].id) - 0.25f) < 1e-3f);
    Render(mixer, 4800);
    view.Tick();
    AETHER_CHECK(std::fabs(view.Playhead() - 0.35f) < 1e-3f);
    view.Stop();
    Render(mixer, 1024);
    AETHER_CHECK(!view.Playing() && mixer.VoiceCount() == 0);
    view.SetSelection(0.5f, 0.55f);
    view.SetPlayhead(0.0f);
    AETHER_CHECK(view.Play()); // outside the selection: starts at its beginning
    for (int i = 0; i < 20; ++i) {
        Render(mixer, 480);
        view.Tick();
        AETHER_CHECK(view.Playhead() >= 0.5f && view.Playhead() < 0.56f);
    }
    AETHER_CHECK(view.Playing());
    // Drawn headless, with and without a sound.
    HeadlessImGui imgui;
    for (int i = 0; i < 3; ++i) imgui.Frame([&] { view.Draw(); });
    WaveformView empty;
    imgui.Frame([&] { empty.Draw(); });
    view.SetSound(&stereo);
    AETHER_CHECK(!view.Playing() && view.ViewFrames() == stereo.Frames());
    imgui.Frame([&] { view.Draw(); });
}

AETHER_TEST(AudioEditor_CueDocumentEditsAndSaves) {
    SoundCueDocument doc;
    doc.SetSounds({"step1", "step2", "step3"});
    AETHER_CHECK(doc.Name() == "Untitled Cue" && !doc.Dirty() && !doc.CanUndo());
    const u32 random = doc.AddNode(CueNodeType::Random, 200, 0);
    AETHER_CHECK(doc.Get().root == random); // the first node plays
    const u32 a = doc.AddNode(CueNodeType::Wave, 0, 0, "step1");
    const u32 b = doc.AddNode(CueNodeType::Wave, 0, 100, "step2");
    const u32 mod = doc.AddNode(CueNodeType::Modulator, 400, 0);
    AETHER_CHECK(doc.Dirty() && doc.Get().nodes.size() == 4 && doc.Get().root == random);
    // Multi-input nodes append (one past the end) or replace; one-input nodes replace.
    std::string error;
    AETHER_CHECK(doc.Connect(a, random, 0, &error) && doc.Connect(b, random, 1, &error));
    AETHER_CHECK(doc.Get().Find(random)->children == (std::vector<u32>{a, b}));
    AETHER_CHECK(!doc.Connect(a, random, 5, &error) && error == "That input doesn't exist.");
    AETHER_CHECK(doc.Connect(b, random, 0) && doc.Get().Find(random)->children == (std::vector<u32>{b, b}));
    AETHER_CHECK(doc.Connect(a, random, 0));
    AETHER_CHECK(doc.Connect(random, mod, 0) && doc.SetRoot(mod));
    AETHER_CHECK(!doc.Connect(a, mod, 1, &error) && error.find("one input") != std::string::npos);
    AETHER_CHECK(!doc.Connect(mod, random, 2, &error) && error.find("loop") != std::string::npos); // mod -> random -> ... -> mod
    AETHER_CHECK(!doc.Connect(random, random, 2, &error));
    AETHER_CHECK(!doc.Connect(random, a, 0, &error) && error.find("Wave") != std::string::npos);
    AETHER_CHECK(!doc.SetRoot(999));
    AETHER_CHECK(doc.ErrorCount() == 0);
    // Disconnect moves later inputs (and their weights) up.
    doc.Edit("Weights", [&](SoundCue& c) { c.Find(random)->weights = {1.0f, 3.0f}; });
    AETHER_CHECK(doc.Disconnect(random, 0));
    AETHER_CHECK(doc.Get().Find(random)->children == (std::vector<u32>{b}) && doc.Get().Find(random)->weights == (std::vector<f32>{3.0f}));
    AETHER_CHECK(!doc.Disconnect(random, 3));
    // Undo walks back one edit at a time; edits that change nothing aren't steps.
    AETHER_CHECK(doc.UndoLabel() == "Break link" && doc.Undo() && doc.Get().Find(random)->children.size() == 2);
    AETHER_CHECK(doc.Redo() && doc.Get().Find(random)->children.size() == 1);
    const std::string before = doc.UndoLabel();
    doc.Edit("Nothing", [](SoundCue&) {});
    AETHER_CHECK(doc.UndoLabel() == before);
    // Deleting unhooks the node everywhere, and from the output.
    doc.DeleteNodes({b});
    AETHER_CHECK(doc.Get().Find(random)->children.empty() && doc.Get().Find(random)->weights.empty());
    AETHER_CHECK(doc.ErrorCount() > 0); // a Random with no inputs
    doc.DeleteNodes({mod});
    AETHER_CHECK(doc.Get().root == 0 && doc.Get().Find(mod) == nullptr);
    doc.Undo();
    doc.Undo();
    AETHER_CHECK(doc.Get().root == mod && doc.Get().Find(random)->children.size() == 1);
    // Unknown sounds are errors once the document knows the sounds.
    const u32 bad = doc.AddNode(CueNodeType::Wave, 0, 200, "nope");
    const auto& d = doc.Diagnostics();
    AETHER_CHECK(std::any_of(d.begin(), d.end(), [&](const CueDiagnostic& x) { return x.code == "CU007" && x.node == bad; }));
    doc.DeleteNodes({bad});

    // Save and load.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_cue_editor_test";
    std::filesystem::create_directories(dir);
    AETHER_CHECK(!doc.Save(&error)); // no file yet
    AETHER_CHECK(doc.SaveAs(dir / "Footsteps.acue", &error) && !doc.Dirty() && doc.Name() == "Footsteps");
    SoundCueDocument loaded;
    AETHER_CHECK(loaded.Load(dir / "Footsteps.acue", &error) && SaveCue(loaded.Get()) == SaveCue(doc.Get()) && !loaded.CanUndo());
    AETHER_CHECK(!loaded.Load(dir / "missing.acue", &error) && !error.empty());
    std::filesystem::remove_all(dir);
}

AETHER_TEST(AudioEditor_CueGraphViewAndEdits) {
    SoundCueDocument doc;
    const u32 cat = doc.AddNode(CueNodeType::Concatenator, 300, 0);
    const u32 a = doc.AddNode(CueNodeType::Wave, 0, 0, "intro");
    const u32 loop = doc.AddNode(CueNodeType::Loop, 100, 100);
    const u32 b = doc.AddNode(CueNodeType::Wave, 0, 200, "body");
    doc.Connect(a, cat, 0);
    doc.Connect(loop, cat, 1);
    doc.Connect(b, loop, 0);
    doc.Edit("Loop", [&](SoundCue& c) { c.Find(b)->looping = true; });

    // Pins: one input for Loop, numbered inputs plus "+" for Concatenator, none for a Wave.
    AETHER_CHECK(CueInputPins(*doc.Get().Find(loop)) == (std::vector<std::string>{"in"}));
    AETHER_CHECK(CueInputPins(*doc.Get().Find(cat)) == (std::vector<std::string>{"in0", "in1", "add"}));
    AETHER_CHECK(CueInputPins(*doc.Get().Find(a)).empty());
    AETHER_CHECK(CueInputIndex(*doc.Get().Find(cat), "in1") == 1 && CueInputIndex(*doc.Get().Find(cat), "add") == 2);
    AETHER_CHECK(CueInputIndex(*doc.Get().Find(cat), "in7") == -1 && CueInputIndex(*doc.Get().Find(loop), "in0") == -1);
    AETHER_CHECK(CueNodeTitle(*doc.Get().Find(b)) == "Wave: body (loop)" && CueNodeTitle(*doc.Get().Find(loop)) == "Loop forever");

    const GraphViewModel m = BuildCueGraphView(doc.Get(), doc.Diagnostics(), 600, 0);
    AETHER_CHECK(m.nodes.size() == 5 && m.Find(kCueOutputNode) != nullptr && m.Find(kCueOutputNode)->x == 600);
    AETHER_CHECK(m.links.size() == 4); // a->cat, loop->cat, b->loop, cat->Output
    AETHER_CHECK(std::any_of(m.links.begin(), m.links.end(), [&](const GraphLinkView& l) { return l.from_node == cat && l.to_node == kCueOutputNode; }));
    AETHER_CHECK(!m.Find(cat)->inputs.back().connected && m.Find(cat)->inputs.back().label == "+");
    // A node with an error carries it (CU010: the loop never ends, so nothing after it plays).
    doc.Connect(a, cat, 2);
    const GraphViewModel warned = BuildCueGraphView(doc.Get(), doc.Diagnostics(), 600, 0);
    AETHER_CHECK(warned.Find(cat)->error.empty()); // CU010 is a warning
    const u32 empty_mix = doc.AddNode(CueNodeType::Mix, 0, 400);
    AETHER_CHECK(BuildCueGraphView(doc.Get(), doc.Diagnostics(), 600, 0).Find(empty_mix)->error.find("CU005") == 0);

    // What the widget reports, applied.
    f32 ox = 600, oy = 0;
    GraphViewResult r;
    r.connect = true;
    r.connect_from = {a, "out", true};
    r.connect_to = {empty_mix, "add", false};
    AETHER_CHECK(ApplyCueGraphEdits(doc, r, ox, oy).empty() && doc.Get().Find(empty_mix)->children == (std::vector<u32>{a}));
    r = {};
    r.connect = true;
    r.connect_from = {empty_mix, "in0", false}; // dragged from the input end
    r.connect_to = {b, "out", true};
    AETHER_CHECK(ApplyCueGraphEdits(doc, r, ox, oy).empty() && doc.Get().Find(empty_mix)->children == (std::vector<u32>{b}));
    r = {};
    r.connect = true;
    r.connect_from = {empty_mix, "out", true};
    r.connect_to = {kCueOutputNode, "sound", false};
    ApplyCueGraphEdits(doc, r, ox, oy);
    AETHER_CHECK(doc.Get().root == empty_mix);
    r = {};
    r.connect = true;
    r.connect_from = {cat, "out", true};
    r.connect_to = {b, "in", false};
    AETHER_CHECK(ApplyCueGraphEdits(doc, r, ox, oy).size() == 1); // Waves have no inputs
    r = {};
    r.disconnect = true;
    r.disconnected = {empty_mix, "out", kCueOutputNode, "sound", 0, 0.0f};
    ApplyCueGraphEdits(doc, r, ox, oy);
    AETHER_CHECK(doc.Get().root == 0);
    r = {};
    r.disconnect = true;
    r.disconnected = {b, "out", empty_mix, "in0", 0, 0.0f};
    ApplyCueGraphEdits(doc, r, ox, oy);
    AETHER_CHECK(doc.Get().Find(empty_mix)->children.empty());
    r = {};
    r.moved = {{a, {50.0f, 60.0f}}, {kCueOutputNode, {900.0f, 10.0f}}};
    ApplyCueGraphEdits(doc, r, ox, oy);
    AETHER_CHECK(doc.Get().Find(a)->x == 50.0f && ox == 900.0f && oy == 10.0f);
    r = {};
    r.deleted = {empty_mix, kCueOutputNode};
    ApplyCueGraphEdits(doc, r, ox, oy);
    AETHER_CHECK(doc.Get().Find(empty_mix) == nullptr && doc.Get().nodes.size() == 4);

    // The attenuation plot.
    AttenuationSettings lin;
    lin.model = AttenuationModel::Linear;
    lin.min_distance = 0.0f, lin.max_distance = 10.0f;
    const std::vector<f32> curve = AttenuationCurve(lin, 11, 10.0f);
    AETHER_CHECK(curve.size() == 11 && curve[0] == 1.0f && std::fabs(curve[5] - 0.5f) < 1e-4f && curve[10] == 0.0f);
}

AETHER_TEST(AudioEditor_CueEditorDrawsAndPreviews) {
    HeadlessImGui imgui;
    SoundCueDocument doc;
    doc.SetSounds({"blip", "hum"});
    const u32 random = doc.AddNode(CueNodeType::Random, 300, 0);
    const u32 a = doc.AddNode(CueNodeType::Wave, 0, 0, "blip");
    const u32 b = doc.AddNode(CueNodeType::Wave, 0, 100, "hum");
    doc.Connect(a, random, 0);
    doc.Connect(b, random, 1);
    SoundCueEditor editor(doc);
    AETHER_CHECK(editor.OutputX() > 300.0f);
    for (int i = 0; i < 2; ++i) imgui.Frame([&] { editor.Draw(); });
    // Every kind of node's Details, and the output's (with 3D and a custom curve).
    for (CueNodeType t : {CueNodeType::Modulator, CueNodeType::Loop, CueNodeType::Mix, CueNodeType::Delay, CueNodeType::Sequence, CueNodeType::Concatenator}) {
        editor.OpenPalette(0, 300);
        const u32 id = editor.PlaceNode(t);
        AETHER_CHECK(editor.Selected() == id && !editor.PaletteOpen());
        imgui.Frame([&] { editor.Draw(); });
    }
    editor.Select(random);
    imgui.Frame([&] { editor.Draw(); });
    editor.Select(a);
    imgui.Frame([&] { editor.Draw(); });
    doc.Edit("3D", [](SoundCue& c) {
        c.spatial = true;
        c.attenuation.model = AttenuationModel::Custom;
        c.attenuation.curve = {{0.0f, 1.0f}, {0.5f, 0.3f}, {1.0f, 0.0f}};
    });
    editor.Select(kCueOutputNode);
    for (int i = 0; i < 2; ++i) imgui.Frame([&] { editor.Draw(); });
    editor.OpenPalette(10, 10);
    imgui.Frame([&] { editor.Draw(); });
    AETHER_CHECK(editor.PaletteOpen());
    AETHER_CHECK(editor.PlaceNode(CueNodeType::Wave, "hum") != 0 && doc.Get().nodes.back().sound == "hum");
    // Deleting the selected node (here, by undo) drops the selection.
    doc.Undo();
    imgui.Frame([&] { editor.Draw(); });
    AETHER_CHECK(editor.Selected() == 0);

    // Preview through a cue player: errors block it; a clean cue plays.
    Mixer mixer(kRate);
    SoundBank bank;
    bank.Add("blip", Dc(0.2f, 480));
    bank.Add("hum", Dc(0.2f, 4800));
    CuePlayer player(mixer, bank);
    AETHER_CHECK(!editor.PlayPreview()); // no player yet
    editor.SetPreview(&player);
    AETHER_CHECK(doc.ErrorCount() > 0 && !editor.PlayPreview() && editor.Status().find("errors") != std::string::npos);
    std::vector<u32> extra;
    for (const CueNode& n : doc.Get().nodes) {
        if (n.id != random && n.id != a && n.id != b) extra.push_back(n.id);
    }
    doc.DeleteNodes(extra);
    AETHER_CHECK(doc.ErrorCount() == 0 && editor.PlayPreview() && editor.Previewing());
    for (int i = 0; i < 10; ++i) {
        Render(mixer, 480);
        imgui.Frame([&] { editor.Draw(); });
    }
    AETHER_CHECK(!editor.Previewing()); // both sounds are shorter than that
    AETHER_CHECK(editor.PlayPreview());
    editor.StopPreview();
    Render(mixer, 2048);
    AETHER_CHECK(!editor.Previewing() && mixer.VoiceCount() == 0);
    // Saving without a file reports why.
    AETHER_CHECK(!editor.SaveNow() && editor.Status().find("Save failed") == 0);
}

AETHER_TEST(AudioEditor_MixerPanelMetersFadersAndEffects) {
    // Ballistics: rises at once, falls at 24 dB/s; the peak holds for a second.
    MeterBallistics m;
    m.Update(1.0f, 0.1f);
    AETHER_CHECK(m.level_db == 0.0f && m.peak_db == 0.0f);
    m.Update(0.0f, 0.5f);
    AETHER_CHECK(std::fabs(m.level_db + 12.0f) < 1e-3f && m.peak_db == 0.0f); // held
    m.Update(0.0f, 0.6f);
    AETHER_CHECK(m.peak_db < 0.0f && m.peak_db >= m.level_db);
    m.Update(DbToGain(-3.0f), 0.01f);
    AETHER_CHECK(std::fabs(m.level_db + 3.0f) < 1e-3f);

    // Effect settings by kind.
    FilterEffect filter(Biquad::Type::LowPass, 1000.0f);
    CompressorEffect comp;
    ReverbEffect reverb;
    AETHER_CHECK(EffectParams(filter).size() == 3 && EffectParams(comp).size() == 5 && EffectParams(reverb).size() == 5);
    std::vector<f32> v = ReadEffect(comp);
    v[1] = 8.0f;
    WriteEffect(comp, v);
    AETHER_CHECK(comp.ratio == 8.0f && ReadEffect(filter)[0] == 1000.0f);
    WriteEffect(filter, {500.0f, 1.0f, 0.0f});
    AETHER_CHECK(filter.Filter().frequency == 500.0f);

    for (bool threaded : {false, true}) {
        Mixer mixer(kRate);
        mixer.AddDefaultBuses();
        const BusId sfx = mixer.FindBus("SFX");
        mixer.AddEffect(sfx, std::make_unique<FilterEffect>(Biquad::Type::LowPass, 2000.0f));
        mixer.AddEffect(kMasterBus, std::make_unique<CompressorEffect>());
        mixer.AddEffect(kMasterBus, std::make_unique<ReverbEffect>());
        AETHER_CHECK(mixer.EffectCount(kMasterBus) == 2 && mixer.EffectCount(sfx) == 1 && mixer.EffectAt(sfx, 1) == nullptr);
        std::unique_ptr<AudioOutput> output;
        NullBackend* null = nullptr;
        if (threaded) {
            auto backend = std::make_unique<NullBackend>(false);
            null = backend.get();
            output = std::make_unique<AudioOutput>(mixer, std::move(backend));
            AETHER_CHECK(output->Start(256));
        }
        auto render = [&](u32 frames) {
            if (threaded) {
                std::thread audio([&] { null->Pump(frames); });
                audio.join();
            } else {
                Render(mixer, frames);
            }
        };
        MixerPanel panel(mixer);
        const SoundWave tone = GenerateTone(440.0f, 0.5f, kRate, 0.5f);
        PlayParams p;
        p.bus = sfx;
        p.loop = true;
        mixer.Play(&tone, p);
        render(2048);
        // Meters follow the bus.
        panel.UpdateMeters(1.0f / 60.0f);
        // A 0.5 tone panned centre (equal power): 0.354, about -9 dB.
        AETHER_CHECK(panel.Ballistics(sfx, 0).level_db > -10.0f && panel.Ballistics(sfx, 0).level_db < -8.0f);
        AETHER_CHECK(panel.Ballistics(mixer.FindBus("Music"), 0).level_db < -100.0f);
        // Faders and mute.
        panel.SetFader(sfx, -6.0f);
        panel.ToggleMute(mixer.FindBus("Music"));
        AETHER_CHECK(mixer.BusVolume(sfx) == -6.0f && mixer.BusMuted(mixer.FindBus("Music")));
        // Effect settings: known at once on one thread; threaded, after the audio thread answers.
        std::vector<f32> values;
        bool bypass = true;
        const bool known = panel.EffectValues(kMasterBus, 0, values, bypass);
        AETHER_CHECK(known == !threaded);
        if (threaded) {
            render(256);
            AETHER_CHECK(panel.EffectValues(kMasterBus, 0, values, bypass));
        }
        AETHER_CHECK(values.size() == 5 && values[0] == -18.0f && !bypass);
        panel.SetEffectParam(kMasterBus, 0, 0, -30.0f);
        panel.SetBypass(sfx, 0, true);
        panel.EffectValues(sfx, 0, values, bypass); // asks for the filter's settings too
        render(256);
        auto* c = static_cast<CompressorEffect*>(mixer.EffectAt(kMasterBus, 0));
        AETHER_CHECK(c->threshold_db == -30.0f && mixer.EffectAt(sfx, 0)->bypass); // (read after the audio thread is idle)
        AETHER_CHECK(panel.EffectValues(kMasterBus, 0, values, bypass) && values[0] == -30.0f);
        AETHER_CHECK(panel.EffectValues(sfx, 0, values, bypass) && bypass && values[0] == 2000.0f);
        // The voices list.
        AETHER_CHECK(mixer.Voices().size() == 1 && mixer.Voices()[0].time > 0.0f);
        // Drawn headless: each tab.
        HeadlessImGui imgui;
        for (MixerPanel::Tab tab : {MixerPanel::Tab::Buses, MixerPanel::Tab::Effects, MixerPanel::Tab::Voices}) {
            panel.ShowTab(tab);
            for (int i = 0; i < 2; ++i) imgui.Frame([&] { panel.Draw(); });
        }
        // An empty mixer too.
        Mixer quiet(kRate);
        MixerPanel idle(quiet);
        for (MixerPanel::Tab tab : {MixerPanel::Tab::Effects, MixerPanel::Tab::Voices}) {
            idle.ShowTab(tab);
            for (int i = 0; i < 2; ++i) imgui.Frame([&] { idle.Draw(); });
        }
        if (output) output->Stop();
    }
}
