#include "editor_scripts.h"

#include "aether/core/log.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

#ifdef AETHER_EDITOR_SCRIPTING
#include "aether/script/luau_host.h"
#endif

namespace aether::editor {

namespace stdfs = std::filesystem;

#ifdef AETHER_EDITOR_SCRIPTING

struct EditorScripts::Impl {
    ExtensionRegistry& registry;
    stdfs::path folder;
    std::unique_ptr<script::LuauHost> host;
    std::vector<std::string> owners;
    std::vector<int> refs;
    std::map<std::string, std::string> errors; // by owner: the latest problem
    std::string current_owner;                 // the script being run

    explicit Impl(ExtensionRegistry& r) : registry(r) {}

    // Runs a callback; a failure becomes the owner's error (cleared on success).
    bool Invoke(const std::string& owner, int ref, const std::vector<script::ScriptValue>& args = {}) {
        const script::ScriptResult result = host->CallRef(ref, args);
        if (result.ok) {
            errors.erase(owner);
        } else {
            errors[owner] = result.error;
        }
        return result.ok;
    }

    void Install() {
        script::LuauHost::Options options;
        options.instruction_budget = 2'000'000; // a panel runs every frame
        host = std::make_unique<script::LuauHost>(options);
        host->SetPrintHandler([](const std::string& text) { AETHER_LOG_INFO("EditorScript", "%s", text.c_str()); });
        using script::NativeCall;

        host->RegisterNative("editor", "Log", [](NativeCall& c) { AETHER_LOG_INFO("EditorScript", "%s", c.String(0).c_str()); });
        host->RegisterNative("editor", "AddPanel", [this](NativeCall& c) {
            const int ref = c.Function(1);
            if (!c.IsString(0) || ref < 0) return c.Fail("editor.AddPanel(name, function)");
            refs.push_back(ref);
            const std::string owner = current_owner;
            ExtensionPanel panel;
            panel.name = c.String(0);
            panel.owner = owner;
            panel.draw = [this, owner, ref]() {
                if (!Invoke(owner, ref)) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", errors[owner].c_str());
            };
            registry.AddPanel(std::move(panel));
        });
        host->RegisterNative("editor", "AddMenuItem", [this](NativeCall& c) {
            const int ref = c.Function(1);
            if (!c.IsString(0) || ref < 0) return c.Fail("editor.AddMenuItem(path, function [, shortcut])");
            refs.push_back(ref);
            const std::string owner = current_owner;
            ExtensionMenuItem item;
            item.path = c.String(0);
            item.shortcut = c.String(2);
            item.owner = owner;
            item.action = [this, owner, ref]() { Invoke(owner, ref); };
            registry.AddMenuItem(std::move(item));
        });
        host->RegisterNative("editor", "AddAssetType", [this](NativeCall& c) {
            if (!c.IsString(0) || !c.IsString(1)) return c.Fail("editor.AddAssetType(name, extension, newFileText [, function(path)])");
            ExtensionAssetType type;
            type.name = c.String(0);
            type.extension = c.String(1);
            type.new_text = c.String(2);
            type.owner = current_owner;
            if (const int ref = c.Function(3); ref >= 0) {
                refs.push_back(ref);
                const std::string owner = current_owner;
                type.open = [this, owner, ref](const stdfs::path& file) { Invoke(owner, ref, {file.generic_string()}); };
            }
            registry.AddAssetType(std::move(type));
        });

        host->RegisterNative("ui", "Text", [](NativeCall& c) { ImGui::TextUnformatted(c.String(0).c_str()); });
        host->RegisterNative("ui", "TextDisabled", [](NativeCall& c) { ImGui::TextDisabled("%s", c.String(0).c_str()); });
        host->RegisterNative("ui", "Button", [](NativeCall& c) { c.Return(ImGui::Button(c.String(0).c_str())); });
        host->RegisterNative("ui", "Checkbox", [](NativeCall& c) {
            bool value = c.Bool(1);
            const bool changed = ImGui::Checkbox(c.String(0).c_str(), &value);
            c.Return(value);
            c.Return(changed);
        });
        host->RegisterNative("ui", "SliderFloat", [](NativeCall& c) {
            float value = static_cast<float>(c.Number(1));
            const bool changed = ImGui::SliderFloat(c.String(0).c_str(), &value, static_cast<float>(c.Number(2, 0.0)),
                                                    static_cast<float>(c.Number(3, 1.0)));
            c.Return(static_cast<f64>(value));
            c.Return(changed);
        });
        host->RegisterNative("ui", "InputText", [](NativeCall& c) {
            std::string value = c.String(1);
            const bool changed = ImGui::InputText(c.String(0).c_str(), &value);
            c.Return(value);
            c.Return(changed);
        });
        host->RegisterNative("ui", "CollapsingHeader", [](NativeCall& c) { c.Return(ImGui::CollapsingHeader(c.String(0).c_str())); });
        host->RegisterNative("ui", "SameLine", [](NativeCall&) { ImGui::SameLine(); });
        host->RegisterNative("ui", "Separator", [](NativeCall&) { ImGui::Separator(); });
        host->RegisterNative("ui", "Spacing", [](NativeCall&) { ImGui::Spacing(); });
    }

    void Drop() {
        for (const std::string& owner : owners) registry.RemoveOwner(owner);
        owners.clear();
        errors.clear();
        if (host) {
            for (int ref : refs) host->Release(ref);
        }
        refs.clear();
        host.reset();
    }
};

EditorScripts::EditorScripts(ExtensionRegistry& registry) : impl_(std::make_unique<Impl>(registry)) {}
EditorScripts::~EditorScripts() { impl_->Drop(); }
bool EditorScripts::Available() { return true; }

usize EditorScripts::Load(const stdfs::path& folder) {
    impl_->Drop();
    impl_->folder = folder;
    std::vector<stdfs::path> files;
    std::error_code ec;
    if (stdfs::is_directory(folder, ec)) {
        for (stdfs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".luau") files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    impl_->Install();
    usize loaded = 0;
    for (const stdfs::path& file : files) {
        const std::string owner = "script:" + stdfs::relative(file, folder, ec).generic_string();
        std::ifstream in(file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        impl_->current_owner = owner;
        impl_->owners.push_back(owner);
        const script::ScriptResult result = impl_->host->Run(text.str(), owner);
        if (result.ok) {
            ++loaded;
        } else {
            impl_->errors[owner] = result.error;
            impl_->registry.RemoveOwner(owner); // a half-run script leaves nothing behind
        }
    }
    impl_->current_owner.clear();
    return loaded;
}

usize EditorScripts::Reload() { return Load(impl_->folder); }
void EditorScripts::Unload() {
    impl_->Drop();
    impl_->folder.clear();
}
usize EditorScripts::ScriptCount() const {
    usize n = 0;
    for (const std::string& owner : impl_->owners) {
        if (impl_->errors.find(owner) == impl_->errors.end()) ++n;
    }
    return n;
}
std::vector<std::string> EditorScripts::Errors() const {
    std::vector<std::string> out;
    for (const auto& [owner, message] : impl_->errors) out.push_back(owner.substr(7) + ": " + message);
    return out;
}

#else // no Luau

struct EditorScripts::Impl {};
EditorScripts::EditorScripts(ExtensionRegistry&) : impl_(std::make_unique<Impl>()) {}
EditorScripts::~EditorScripts() = default;
bool EditorScripts::Available() { return false; }
usize EditorScripts::Load(const stdfs::path&) { return 0; }
usize EditorScripts::Reload() { return 0; }
void EditorScripts::Unload() {}
usize EditorScripts::ScriptCount() const { return 0; }
std::vector<std::string> EditorScripts::Errors() const { return {"editor scripts need a build with Luau (AETHER_BUILD_SCRIPTING)"}; }

#endif

} // namespace aether::editor
