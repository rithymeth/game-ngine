#include "test_framework.h"
#include "vfx/vfx_editor.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>

#if defined(__linux__)
#include <signal.h>
#include <sys/resource.h>
#endif

// Phase 19 step 6: the particle editor's document (emitters, modules and
// their fields, settings, parameters, bindings that follow edits,
// sub-emitters, checks, undo and files), curve and gradient key editing,
// the deterministic preview with its camera and stats, and the panel
// drawn headless.

using namespace aether;
using namespace aether::editor;
using namespace aether::vfx;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGui::GetIO().ConfigMacOSXBehaviors = false; // tests press Ctrl on every platform
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
        ImGui::Begin("Particles", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

} // namespace

AETHER_TEST(VfxEditor_Document) {
    ParticleSystemDocument doc;
    CHECK(doc.Get().emitters.size() == 1 && doc.EmitterAt(0)->settings.name == "Emitter" && doc.Diagnostics().empty() && !doc.Dirty());
    CHECK(doc.ModuleCount(0, VfxStage::Spawn) == 1 && doc.ModuleCount(0, VfxStage::Init) == 3 && doc.ModuleCount(0, VfxStage::Render) == 1);
    CHECK(StageModuleNames(VfxStage::Update).size() == 11 && std::string(StageKey(VfxStage::Init)) == "init" && std::string(StageLabel(VfxStage::Init)) == "Initialize");
    std::string error;

    // Emitters: unique names, duplicates, renames that sub-emitters follow, moves, removal, undo.
    CHECK(doc.AddEmitter("Emitter") == 1 && doc.EmitterAt(1)->settings.name == "Emitter1");
    CHECK(doc.DuplicateEmitter(0) == std::optional<usize>(1) && doc.EmitterAt(1)->settings.name == "Emitter2" && doc.Get().emitters.size() == 3);
    CHECK(doc.AddSubEmitter(2, {ParticleEventKind::Death, "Emitter"}));
    CHECK(doc.RenameEmitter(0, "Fire") && doc.EmitterAt(2)->sub_emitters[0].emitter == "Fire");
    CHECK(!doc.RenameEmitter(1, "Fire", &error) && error.find("already") != std::string::npos && !doc.RenameEmitter(1, "", &error));
    CHECK(doc.MoveEmitter(0, 2) && doc.EmitterAt(2)->settings.name == "Fire" && doc.RemoveEmitter(0) && doc.Get().emitters.size() == 2);
    CHECK(doc.UndoLabel() == "Remove emitter" && doc.Undo() && doc.Get().emitters.size() == 3 && doc.Redo() && doc.Undo());

    // Settings through their saved form, including the ones files leave out.
    const nlohmann::json settings = doc.EmitterSettings(2);
    CHECK(settings["name"] == "Fire" && settings["enabled"] == true && settings["target"] == "Auto" && settings.contains("max_particles") && !settings.contains("spawn"));
    CHECK(doc.SetEmitterSetting(2, "max_particles", 50) && doc.EmitterAt(2)->settings.max_particles == 50u);
    CHECK(doc.SetEmitterSetting(2, "space", "Local") && doc.EmitterAt(2)->settings.space == SimSpace::Local);
    CHECK(!doc.SetEmitterSetting(2, "space", "Galaxy", &error) && error.find("Galaxy") != std::string::npos);
    CHECK(!doc.SetEmitterSetting(2, "spawn", nlohmann::json::array(), &error) && doc.SetEmitterSetting(2, "name", "Flame") && doc.EmitterAt(2)->settings.name == "Flame");

    // Modules: added by name into the right stage, fields set through their saved form.
    const auto gravity = doc.AddModule(2, VfxStage::Update, "Gravity");
    CHECK(gravity && gravity->index == 0 && doc.ModuleName(*gravity) == "Gravity" && doc.UndoLabel() == "Add Gravity");
    CHECK(!doc.AddModule(2, VfxStage::Update, "SpawnRate", &error) && error.find("no Update module") != std::string::npos && !doc.AddModule(9, VfxStage::Update, "Drag"));
    CHECK(doc.SetModuleField(*gravity, "acceleration", {0, -1, 0}) && std::get<Gravity>(doc.EmitterAt(2)->update[0]).acceleration.y == -1.0f);
    CHECK(!doc.SetModuleField(*gravity, "acceleration", {1, 2}, &error) && error.find("acceleration: a vector is") != std::string::npos);
    CHECK(!doc.SetModuleField(*gravity, "wobble", 1, &error) && error.find("no field 'wobble'") != std::string::npos);
    CHECK(!doc.SetModuleField(*gravity, "module", "Drag", &error));
    const u64 before = doc.Revision();
    CHECK(doc.SetModuleField(*gravity, "acceleration", {0, -1, 0}) && doc.Revision() == before); // no change, no step
    CHECK(doc.SetModuleEnabled(*gravity, false) && doc.ModuleJson(*gravity)["enabled"] == false && doc.SetModuleEnabled(*gravity, true) && !doc.ModuleJson(*gravity).contains("enabled"));
    // Merged drags.
    for (int i = 1; i <= 5; ++i) doc.SetModuleField(*gravity, "acceleration", {0, -static_cast<f32>(i), 0}, nullptr, "drag");
    CHECK(doc.Undo() && std::get<Gravity>(doc.EmitterAt(2)->update[0]).acceleration.y == -1.0f && doc.Redo());

    // Parameters, and bindings that follow module moves and parameter renames.
    CHECK(doc.AddParameter("Wind", ParameterValue::Vector({1, 0, 0})) && doc.AddParameter("Rate", ParameterValue::Float(10)));
    CHECK(!doc.AddParameter("Wind", ParameterValue::Float(1), &error) && !doc.AddParameter("", ParameterValue::Float(1), &error));
    const auto drag = doc.AddModule(2, VfxStage::Update, "Drag");
    CHECK(drag && drag->index == 1);
    CHECK(doc.AddBinding(2, {"Wind", "update[0].acceleration"}) && doc.AddBinding(2, {"Rate", "spawn[0].rate"}) && doc.AddBinding(2, {"Wind", "update[1].coefficient"}));
    const auto moved = doc.MoveModule(*gravity, 1);
    CHECK(moved && moved->index == 1 && doc.EmitterAt(2)->bindings[0].field == "update[1].acceleration" && doc.EmitterAt(2)->bindings[2].field == "update[0].coefficient");
    CHECK(doc.ModuleName({2, VfxStage::Update, 0}) == "Drag");
    CHECK(doc.RemoveModule(*moved) && doc.EmitterAt(2)->bindings.size() == 2 && doc.EmitterAt(2)->bindings[1].field == "update[0].coefficient");
    CHECK(doc.RenameParameter("Rate", "Intensity") && doc.EmitterAt(2)->bindings[0].parameter == "Intensity" && !doc.RenameParameter("Intensity", "Wind", &error));
    CHECK(doc.SetParameterDefault("Intensity", ParameterValue::Float(25)) && doc.Get().FindParameter("Intensity")->value.f == 25.0f);
    CHECK(doc.RemoveParameter("Intensity") && doc.EmitterAt(2)->bindings.size() == 1 && !doc.RemoveParameter("Intensity"));
    CHECK(doc.RemoveBinding(2, 0) && doc.EmitterAt(2)->bindings.empty() && !doc.RemoveBinding(2, 0));

    // Sub-emitters and checks.
    CHECK(doc.SetSubEmitter(1, 0, {ParticleEventKind::Collision, "Nowhere"}) && doc.EmitterAt(1)->sub_emitters[0].event == ParticleEventKind::Collision);
    CHECK(doc.ErrorCount() == 1 && doc.Diagnostics()[0].code == "FX011");
    CHECK(doc.RemoveSubEmitter(1, 0) && doc.ErrorCount() == 0 && !doc.RemoveSubEmitter(1, 0));

    // Files.
    CHECK(!doc.Save(&error) && error.find("Save As") != std::string::npos);
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "aether_vfx_editor_test.avfx";
    CHECK(doc.SaveAs(file, &error) && !doc.Dirty() && doc.Name() == "aether_vfx_editor_test");
    ParticleSystemDocument loaded;
    CHECK(loaded.Load(file, &error) && loaded.Text() == doc.Text() && !loaded.CanUndo());
    std::filesystem::remove(file);

    // Curve and gradient keys.
    FloatCurve c = FloatCurve::Constant(1.0f);
    CHECK(AddCurveKey(c, 1.0f, 0.0f) == 1 && AddCurveKey(c, 0.5f, 2.0f) == 1 && c.keys.size() == 3 && c.Sorted());
    CHECK(AddCurveKey(c, 0.5004f, 3.0f) == 1 && c.keys.size() == 3 && c.keys[1].value == 3.0f); // replaced
    CHECK(AddCurveKey(c, 2.0f, 5.0f) == 2 && c.keys[2].time == 1.0f && c.keys[2].value == 5.0f); // clamped onto the end key
    CHECK(MoveCurveKey(c, 0, 0.9f, 7.0f) == 1 && c.keys[1].time == 0.9f && Near(c.keys[0].time, 0.5f) && c.Sorted());
    CHECK(RemoveCurveKey(c, 0) && RemoveCurveKey(c, 0) && !RemoveCurveKey(c, 0) && c.keys.size() == 1);
    ColorGradient g = ColorGradient::Constant({1, 1, 1, 1});
    CHECK(AddColorKey(g, 1.0f, {1, 0, 0, 1}) == 1 && Near(g.Evaluate(0.5f).g, 0.5f) && MoveColorKey(g, 1, 0.0f) == 0 && g.colors.size() == 1);
    CHECK(AddAlphaKey(g, 1.0f, 0.0f) == 1 && Near(g.Evaluate(0.5f).a, 0.5f) && RemoveAlphaKey(g, 1) && !RemoveAlphaKey(g, 0) && !RemoveColorKey(g, 0));
}

#if defined(__linux__)
AETHER_TEST(VfxEditor_FailedSavePreservesParticleAssets) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "aether_vfx_write_failure";
    fs::create_directories(dir);
    const auto original = dir / "Sparks.avfx";
    const auto other = dir / "Backup.avfx";
    const auto read = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };

    ParticleSystemDocument doc;
    CHECK(doc.SaveAs(other) && doc.SaveAs(original));
    const auto original_before = read(original);
    const auto other_before = read(other);
    CHECK(!original_before.empty() && !other_before.empty());
    CHECK(doc.AddEmitter("Smoke") == 1);
    const auto pending = doc.Text();

    struct rlimit previous_limit{};
    struct sigaction previous_signal{}, ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    const bool read_limit = getrlimit(RLIMIT_FSIZE, &previous_limit) == 0;
    CHECK(read_limit);
    if (!read_limit) return;
    const bool changed_signal = sigaction(SIGXFSZ, &ignore, &previous_signal) == 0;
    CHECK(changed_signal);
    if (!changed_signal) return;
    const struct rlimit blocked{0, previous_limit.rlim_max};
    const bool limited = setrlimit(RLIMIT_FSIZE, &blocked) == 0;
    std::string save_error, copy_error;
    const bool saved = limited && doc.Save(&save_error);
    const bool copied = limited && doc.SaveAs(other, &copy_error);
    const bool restored = !limited || setrlimit(RLIMIT_FSIZE, &previous_limit) == 0;
    const bool signal_restored = sigaction(SIGXFSZ, &previous_signal, nullptr) == 0;
    CHECK(limited && restored && signal_restored);

    CHECK(!saved && !save_error.empty() && !copied && !copy_error.empty());
    CHECK(read(original) == original_before && read(other) == other_before);
    CHECK(doc.Path() == original && doc.Dirty() && doc.CanUndo() && doc.Text() == pending);
    CHECK(doc.Save() && !doc.Dirty());
    ParticleSystemDocument reloaded;
    CHECK(reloaded.Load(original) && reloaded.Text() == pending && read(other) == other_before);
    CHECK(std::distance(fs::directory_iterator(dir), fs::directory_iterator{}) == 2);
    fs::remove_all(dir);
}
#endif

AETHER_TEST(VfxEditor_HistoryClearsStaleEmitterAndModuleSelection) {
    HeadlessImGui ui;
    ParticleSystemDocument doc;
    CHECK(doc.AddEmitter("Smoke") == 1);
    CHECK(doc.MoveEmitter(1, 0));
    ParticleEditor editor(doc);
    const auto frame = [&] { ui.Frame([&] { editor.Draw(); }); };
    frame();
    const auto shortcut = [&](ImGuiKey key) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(key, true);
        frame();
        io.AddKeyEvent(key, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        frame();
    };

    const ModuleRef first{0, VfxStage::Spawn, 0};
    editor.SelectModule(first);
    CHECK(editor.SelectedModule() == first);
    shortcut(ImGuiKey_Z);
    CHECK(doc.EmitterAt(0) != nullptr && doc.EmitterAt(0)->settings.name == "Emitter");
    CHECK(!editor.SelectedModule() && editor.SelectedEmitter() < 0);

    editor.SelectModule(first);
    shortcut(ImGuiKey_Y);
    CHECK(doc.EmitterAt(0) != nullptr && doc.EmitterAt(0)->settings.name == "Smoke");
    CHECK(!editor.SelectedModule() && editor.SelectedEmitter() < 0);

    editor.SelectModule(first);
    shortcut(ImGuiKey_Y); // no remaining redo; keep the current selection
    CHECK(editor.SelectedModule() == first && editor.SelectedEmitter() == 0);
}

AETHER_TEST(VfxEditor_ModuleSelectionFollowsMovedEmitter) {
    HeadlessImGui ui;
    ParticleSystemDocument doc;
    CHECK(doc.AddEmitter("Smoke") == 1);
    ParticleEditor editor(doc);
    editor.SelectModule({1, VfxStage::Spawn, 0});

    ImGuiWindow* emitters = nullptr;
    const auto draw = [&] {
        const ImGuiID id = ImGui::GetID("emitters");
        const auto* parent = ImGui::GetCurrentWindowRead();
        editor.Draw();
        for (auto* child : parent->DC.ChildWindows)
            if (child->ChildId == id) emitters = child;
    };
    ui.Frame(draw);
    if (!emitters) { CHECK(emitters != nullptr); return; }

    auto& io = ImGui::GetIO();
    const auto click_arrow = [&](const char* name, int column) {
        const auto& style = ImGui::GetStyle();
        const f32 h = ImGui::GetFrameHeight();
        const ImGuiID id = emitters->GetID(name);
        const ImVec2 origin = emitters->DC.CursorStartPos;
        const f32 x = origin.x + h * 0.5f + static_cast<f32>(column) * (h + style.ItemSpacing.x);
        const f32 end = std::min(emitters->InnerClipRect.Max.y, origin.y + ImGui::GetFrameHeightWithSpacing() * 7.0f);
        for (f32 y = origin.y; y < end; y += 3.0f) {
            io.AddMousePosEvent(x, y);
            ui.Frame(draw);
            if (ImGui::GetHoveredID() != id) continue;
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            ui.Frame(draw);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            ui.Frame(draw);
            return true;
        }
        return false;
    };

    CHECK(click_arrow("up", 0));
    CHECK(doc.EmitterAt(0) != nullptr && doc.EmitterAt(0)->settings.name == "Smoke");
    CHECK(editor.SelectedEmitter() == 0 && editor.SelectedModule() == ModuleRef{0, VfxStage::Spawn, 0});

    CHECK(click_arrow("down", 1));
    CHECK(doc.EmitterAt(1) != nullptr && doc.EmitterAt(1)->settings.name == "Smoke");
    CHECK(editor.SelectedEmitter() == 1 && editor.SelectedModule() == ModuleRef{1, VfxStage::Spawn, 0});

    // Moving once more and deleting must edit Smoke, not the emitter that inherited its old slot.
    CHECK(click_arrow("up", 0));
    io.AddKeyEvent(ImGuiKey_Delete, true);
    ui.Frame(draw);
    io.AddKeyEvent(ImGuiKey_Delete, false);
    ui.Frame(draw);
    CHECK(doc.ModuleCount(0, VfxStage::Spawn) == 0 && doc.ModuleCount(1, VfxStage::Spawn) == 1);
}

AETHER_TEST(VfxEditor_LoadingAnotherAssetResetsSelectionAndPreview) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "aether_vfx_asset_switch";
    fs::create_directories(dir);
    ParticleSystemDocument doc, second;
    CHECK(doc.SaveAs(dir / "First.avfx"));
    CHECK(second.RenameEmitter(0, "Smoke") && second.SaveAs(dir / "Second.avfx"));

    HeadlessImGui ui;
    ParticleEditor editor(doc);
    const auto frame = [&] { ui.Frame([&] { editor.Draw(); }); };
    editor.Pause();
    const ModuleRef selected{0, VfxStage::Spawn, 0};
    editor.SelectModule(selected);
    editor.SetTime(2.0f);
    CHECK(Near(editor.Time(), 2.0f));

    std::string error;
    CHECK(!doc.Load(dir / "Missing.avfx", &error) && !error.empty());
    frame();
    CHECK(editor.SelectedModule() == selected && Near(editor.Time(), 2.0f));
    CHECK(doc.SaveAs(dir / "Copy.avfx"));
    frame();
    CHECK(editor.SelectedModule() == selected && Near(editor.Time(), 2.0f));

    CHECK(doc.Load(dir / "Second.avfx", &error));
    frame();
    CHECK(!editor.SelectedModule() && editor.SelectedEmitter() < 0);
    CHECK(editor.Time() == 0.0f && !editor.Playing());
    const auto stats = editor.Preview().Stats();
    CHECK(stats.size() == 1 && stats.front().name == "Smoke");
    fs::remove_all(dir);
}

AETHER_TEST(VfxEditor_PreviewIgnoresNonFiniteFrameTime) {
    ParticleSystemDocument doc;
    ParticlePreview preview;
    preview.Reset(doc.Get());
    const f32 half = ParticlePreview::kStep * 0.5f;
    preview.Advance(half);
    preview.Advance(std::numeric_limits<f32>::quiet_NaN());
    preview.Advance(half);
    CHECK(Near(preview.Time(), ParticlePreview::kStep, 1e-6f));
    // Keep the pre-fix regression bounded: an infinite delta used to trap the step loop.
    if (!Near(preview.Time(), ParticlePreview::kStep, 1e-6f)) return;
    preview.Advance(std::numeric_limits<f32>::infinity());
    CHECK(Near(preview.Time(), ParticlePreview::kStep, 1e-6f));
    preview.Advance(ParticlePreview::kStep);
    CHECK(Near(preview.Time(), 2.0f * ParticlePreview::kStep, 1e-6f));
}

AETHER_TEST(VfxEditor_PreviewAndPanel) {
    // A fountain: rate, upward cone, gravity.
    ParticleSystemDocument doc;
    CHECK(doc.SetModuleField({0, VfxStage::Spawn, 0}, "rate", 30.0f));
    const auto gravity = doc.AddModule(0, VfxStage::Update, "Gravity");
    CHECK(gravity.has_value());

    // The preview: whole steps, and seeking replays exactly.
    ParticlePreview a, b;
    a.Reset(doc.Get());
    a.Advance(0.01f);
    CHECK(a.Time() == 0.0f && a.Count() == 0); // less than a step: carried
    a.Advance(1.0f - 0.01f);
    CHECK(Near(a.Time(), 1.0f, 1e-4f) && a.Count() == 30);
    b.Seek(doc.Get(), 1.0f);
    bool same = a.Count() == b.Count();
    for (usize i = 0; same && i < a.Count(); ++i) {
        const Vec3 pa = a.Instance()->Emitter(0).Particles().position[i], pb = b.Instance()->Emitter(0).Particles().position[i];
        same &= Near(pa.x, pb.x, 1e-5f) && Near(pa.y, pb.y, 1e-5f) && Near(pa.z, pb.z, 1e-5f);
    }
    CHECK(same);
    b.Seek(doc.Get(), 1000.0f);
    CHECK(Near(b.Time(), ParticlePreview::kMaxSeek, 0.02f));
    // Stats and the camera.
    const auto stats = a.Stats();
    CHECK(stats.size() == 1 && stats[0].name == "Emitter" && stats[0].count == 30 && stats[0].spawned == 30 && stats[0].bounds.valid && stats[0].target == SimTarget::Cpu &&
          stats[0].gpu_supported && stats[0].update_ms >= 0.0);
    f32 x = 0, y = 0, depth = 0;
    CHECK(a.Project(a.target, 800, 600, x, y, depth) && Near(x, 400, 0.5f) && Near(y, 300, 0.5f) && Near(depth, a.distance, 0.01f));
    const ParticleCamera cam = a.Camera();
    const Vec3 to_target = (a.target - cam.position).Normalized();
    CHECK(Near(cam.forward.Dot(to_target), 1.0f, 1e-4f) && Near((a.target - cam.position).Length(), a.distance, 1e-3f));

    // The panel, drawn headless.
    HeadlessImGui imgui;
    ParticleEditor editor(doc);
    auto frame = [&] { imgui.Frame([&] { editor.Draw(); }); };
    for (int i = 0; i < 30; ++i) frame(); // playing
    CHECK(editor.Playing() && Near(editor.Time(), 0.5f, 0.02f) && editor.Preview().Count() == 15);
    editor.Pause();
    frame();
    const f32 paused = editor.Time();
    frame();
    CHECK(editor.Time() == paused);
    editor.SetTime(2.0f);
    CHECK(Near(editor.Time(), 2.0f, 1e-3f) && editor.Preview().Count() > 30);
    // An edit shows at once, at the same moment of the preview.
    const usize before = editor.Preview().Count();
    CHECK(doc.SetModuleField({0, VfxStage::Spawn, 0}, "rate", 60.0f));
    frame();
    CHECK(Near(editor.Time(), 2.0f, 1e-3f) && editor.Preview().Count() > before);
    // Details for every kind of module, the emitter, and each tab: no trouble drawing any of them.
    const auto size = doc.AddModule(0, VfxStage::Update, "SizeOverLife");
    const auto color = doc.AddModule(0, VfxStage::Update, "ColorOverLife");
    const auto shape = doc.AddModule(0, VfxStage::Init, "InitShape");
    const auto ribbon = doc.AddModule(0, VfxStage::Render, "RibbonRenderer");
    const auto light = doc.AddModule(0, VfxStage::Render, "LightRenderer");
    CHECK(size && color && shape && ribbon && light);
    for (const ModuleRef& ref : {*size, *color, *shape, *ribbon, *gravity, ModuleRef{0, VfxStage::Init, 0}}) {
        editor.SelectModule(ref);
        frame();
        CHECK(editor.SelectedModule() && *editor.SelectedModule() == ref);
    }
    editor.SelectEmitter(0);
    doc.AddParameter("Tint", ParameterValue::Color({1, 0, 0, 1}));
    frame();
    CHECK(!editor.SelectedModule() && editor.SelectedEmitter() == 0);
    // The light and ribbon keep it on the CPU.
    CHECK(!editor.Preview().Stats()[0].gpu_supported);
    // Keys: Delete removes the selected module, Ctrl+Z brings it back; a stale selection is dropped.
    editor.SelectModule(*light);
    frame();
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Delete, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Delete, false);
    frame();
    CHECK(doc.ModuleCount(0, VfxStage::Render) == 2 && !editor.SelectedModule());
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_Z, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Z, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame();
    CHECK(doc.ModuleCount(0, VfxStage::Render) == 3);
    editor.SelectModule({0, VfxStage::Render, 2});
    doc.Undo(); // the Tint parameter
    doc.Undo(); // the light's addition
    frame();
    CHECK(!editor.SelectedModule());
    // Looping: the preview starts over.
    editor.loop_length = 0.5f;
    editor.Play();
    for (int i = 0; i < 40; ++i) frame();
    CHECK(editor.Time() < 0.5f);
    CHECK(editor.SaveNow() == false && !editor.Status().empty());
}
