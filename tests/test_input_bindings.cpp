#include "aether/input/bindings.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>

using namespace aether;
using namespace aether::input;
namespace stdfs = std::filesystem;

namespace {

InputBinding Bind(const std::string& action, Key key) {
    InputBinding b;
    b.action = action;
    b.key = key;
    return b;
}

InputMappingContext OnFoot() {
    InputMappingContext context;
    context.name = "OnFoot";
    context.bindings = {Bind("Jump", Key::Space), Bind("Fire", Key::MouseLeft), Bind("Jump", Key::GamepadA),
                        Bind("Crouch", Key::C)};
    return context;
}

stdfs::path Fresh(const char* name) {
    stdfs::path dir = stdfs::temp_directory_path() / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir / "Content/Input");
    return dir;
}

} // namespace

AETHER_TEST(InputAssets_SaveLoadAndActivateFromTheDatabase) {
    const stdfs::path project = Fresh("aether_test_input_assets");
    const stdfs::path content = project / "Content";
    std::string error;
    AETHER_CHECK(SaveInputAction({"Jump", ActionValueType::Bool}, content / "Input/Jump.aaction", &error));
    AETHER_CHECK(SaveInputAction({"Fire", ActionValueType::Bool}, content / "Input/Fire.aaction", &error));
    AETHER_CHECK(SaveMappingContext(OnFoot(), content / "Input/OnFoot.amapping", &error));
    InputMappingContext unnamed;
    unnamed.bindings = {Bind("Fire", Key::Enter)};
    AETHER_CHECK(SaveMappingContext(unnamed, content / "Input/Menu.amapping", &error));
    std::ofstream(content / "Input/Broken.aaction") << "{ not json";

    // Round trip of one file.
    InputMappingContext loaded;
    AETHER_CHECK(LoadMappingContext(content / "Input/OnFoot.amapping", loaded, &error));
    AETHER_CHECK(loaded.name == "OnFoot" && loaded.bindings.size() == 4 && loaded.bindings[2].key == Key::GamepadA);
    InputAction wrong_kind;
    AETHER_CHECK(!LoadInputAction(content / "Input/OnFoot.amapping", wrong_kind, &error)); // a context isn't an action

    // Through the database: the extensions are recognised.
    assets::AssetDatabase db(content);
    db.Scan();
    AETHER_CHECK(db.FindByPath("Input/Jump.aaction")->importer == "InputAction");
    AETHER_CHECK(db.FindByPath("Input/OnFoot.amapping")->importer == "InputMapping");
    InputAssetLibrary library;
    AETHER_CHECK(library.Load(db) == 1 && library.Errors()[0].find("Broken") != std::string::npos);
    AETHER_CHECK(library.Actions().size() == 2);
    AETHER_CHECK((library.ContextNames() == std::vector<std::string>{"Menu", "OnFoot"})); // unnamed: named after its file

    InputSystem system;
    library.RegisterActions(system);
    AETHER_CHECK(system.FindAction("Jump") != nullptr);
    AETHER_CHECK(library.Activate(system, "OnFoot", 0) && system.HasContext("OnFoot"));
    AETHER_CHECK(!library.Activate(system, "Nope", 0));
    InputState state;
    state.SetButton(Key::GamepadA, true);
    system.Update(state, 0.1f);
    AETHER_CHECK(system.IsTriggered("Jump"));
    stdfs::remove_all(project);
}

AETHER_TEST(InputBindings_RebindConflictsAndPersist) {
    const InputMappingContext defaults = OnFoot();
    AETHER_CHECK(FindBindingSlot(defaults, "Jump", 0) == 0 && FindBindingSlot(defaults, "Jump", 1) == 2);
    AETHER_CHECK(FindBindingSlot(defaults, "Jump", 2) == -1 && FindBindingSlot(defaults, "Dance", 0) == -1);

    // Moving Jump's keyboard binding to C conflicts with Crouch.
    auto conflicts = FindConflicts(defaults, Key::C, "Jump", 0);
    AETHER_CHECK(conflicts.size() == 1 && conflicts[0].first == "Crouch" && conflicts[0].second == 0);
    AETHER_CHECK(FindConflicts(defaults, Key::Space, "Jump", 0).empty()); // its own current key
    AETHER_CHECK(FindConflicts(defaults, Key::None).empty());

    UserBindings user;
    user.Set("OnFoot", "Jump", 0, Key::F);
    user.Set("OnFoot", "Jump", 0, Key::J); // replaces
    user.Set("OnFoot", "Fire", 0, Key::None); // unbound
    user.Set("OnFoot", "Jump", 5, Key::K);    // no such binding: stale
    user.Set("Vehicle", "Horn", 0, Key::H);   // another context: ignored here
    AETHER_CHECK(user.overrides.size() == 4 && user.Find("OnFoot", "Jump", 0)->key == Key::J);
    std::vector<KeyOverride> stale;
    const InputMappingContext mine = ApplyUserBindings(defaults, user, &stale);
    AETHER_CHECK(mine.bindings[0].key == Key::J && mine.bindings[1].key == Key::None);
    AETHER_CHECK(mine.bindings[2].key == Key::GamepadA && defaults.bindings[0].key == Key::Space); // defaults untouched
    AETHER_CHECK(stale.size() == 1 && stale[0].slot == 5);

    // The rebind takes effect in play; the old key no longer jumps.
    InputSystem system;
    system.AddContext(mine);
    InputState state;
    state.SetButton(Key::Space, true);
    system.Update(state, 0.1f);
    AETHER_CHECK(!system.IsTriggered("Jump"));
    state.SetButton(Key::J, true);
    system.Update(state, 0.1f);
    AETHER_CHECK(system.IsTriggered("Jump"));

    // Saved per player, and a missing file means no rebinds.
    const stdfs::path saved = Fresh("aether_test_user_bindings") / "Saved";
    const stdfs::path file = UserBindingsPath(saved);
    AETHER_CHECK(file == saved / "Config" / "Input.json");
    UserBindings none;
    none.Set("x", "y", 0, Key::A);
    AETHER_CHECK(LoadUserBindings(file, none) && none.overrides.empty());
    std::string error;
    AETHER_CHECK(SaveUserBindings(user, file, &error));
    UserBindings reloaded;
    AETHER_CHECK(LoadUserBindings(file, reloaded, &error) && reloaded.overrides.size() == 4);
    AETHER_CHECK(reloaded.Find("OnFoot", "Jump", 0)->key == Key::J && reloaded.Find("OnFoot", "Fire", 0)->key == Key::None);
    std::ifstream text(file);
    std::string contents((std::istreambuf_iterator<char>(text)), std::istreambuf_iterator<char>());
    AETHER_CHECK(contents.find("\"J\"") != std::string::npos); // keys saved by name
    text.close(); // Windows does not allow deleting a directory with an open file.

    // Reset one, then all.
    AETHER_CHECK(reloaded.Reset("OnFoot", "Jump", 0) && !reloaded.Reset("OnFoot", "Jump", 0));
    AETHER_CHECK(ApplyUserBindings(defaults, reloaded).bindings[0].key == Key::Space);
    reloaded.ResetAll();
    AETHER_CHECK(reloaded.overrides.empty());
    stdfs::remove_all(saved.parent_path());
}

AETHER_TEST(InputBindings_KeyCapture) {
    InputState state;
    KeyCapture capture;
    Key key = Key::None;
    AETHER_CHECK(capture.Update(state, key) == KeyCapture::Result::Canceled); // not started

    // The mouse button that clicked "Press a key..." is still down: ignored
    // until released; mouse movement never counts.
    state.SetButton(Key::MouseLeft, true);
    capture.Begin(state);
    AETHER_CHECK(capture.IsActive() && capture.Update(state, key) == KeyCapture::Result::Waiting);
    state.AddMouseDelta(50.0f, 20.0f);
    AETHER_CHECK(capture.Update(state, key) == KeyCapture::Result::Waiting);
    state.EndFrame();
    state.SetButton(Key::MouseLeft, false);
    AETHER_CHECK(capture.Update(state, key) == KeyCapture::Result::Waiting);
    // A small stick wobble doesn't count; pushing it does.
    state.SetAxis(Key::GamepadLeftTrigger, 0.3f);
    AETHER_CHECK(capture.Update(state, key) == KeyCapture::Result::Waiting);
    state.SetAxis(Key::GamepadLeftTrigger, 0.9f);
    AETHER_CHECK(capture.Update(state, key) == KeyCapture::Result::Captured && key == Key::GamepadLeftTrigger);
    AETHER_CHECK(!capture.IsActive());

    // Escape cancels, unless it's allowed as a binding.
    state.Clear();
    capture.Begin(state);
    state.SetButton(Key::Escape, true);
    AETHER_CHECK(capture.Update(state, key) == KeyCapture::Result::Canceled && !capture.IsActive());
    state.Clear();
    capture.Begin(state, /*escape_cancels=*/false);
    state.SetButton(Key::Escape, true);
    AETHER_CHECK(capture.Update(state, key) == KeyCapture::Result::Captured && key == Key::Escape);
    // Re-pressing the previously held button after release captures it.
    state.Clear();
    state.SetButton(Key::MouseLeft, true);
    capture.Begin(state);
    state.SetButton(Key::MouseLeft, false);
    capture.Update(state, key);
    state.SetButton(Key::MouseLeft, true);
    AETHER_CHECK(capture.Update(state, key) == KeyCapture::Result::Captured && key == Key::MouseLeft);
}
