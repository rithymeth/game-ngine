#include "core/scene_autosave.h"

#include "core/recent_projects.h"
#include "core/scene_document.h"

#include "aether/platform/filesystem.h"
#include "aether/reflection/serialize.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <system_error>

namespace aether::editor {

namespace stdfs = std::filesystem;

namespace {

// FNV-1a, 64 bit: the same project path always gives the same folder, and two paths that differ in more than
// case (the same project spelled another way) do not.
u64 PathHash(const std::string& text) {
    u64 hash = 1469598103934665603ull;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string SlotName(const stdfs::path& project) {
    std::string stem = project.stem().string();
    for (char& c : stem) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    if (stem.empty()) stem = "project";
    std::string path = project.lexically_normal().generic_string();
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(PathHash(path)));
    return stem + "-" + hex;
}

std::string UtcNow() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M UTC", &tm);
    return text;
}

} // namespace

stdfs::path SceneAutosaves::DefaultBaseDir() {
    if (const char* base = std::getenv("AETHER_EDITOR_AUTOSAVE_DIR"); base != nullptr && *base != '\0') return base;
    return RecentProjects::DefaultConfigDir() / "Autosave";
}

SceneAutosaves::SceneAutosaves(stdfs::path base) : base_(base.empty() ? DefaultBaseDir() : std::move(base)) {}

SceneAutosaveSlot SceneAutosaves::SlotFor(const stdfs::path& project) const {
    SceneAutosaveSlot slot;
    if (project.empty()) return slot;
    const stdfs::path folder = base_ / SlotName(project);
    slot.scene = folder / "recovery.ascene";
    slot.metadata = folder / "recovery.json";
    return slot;
}

bool SceneAutosaves::Write(const SceneDocument& document, const stdfs::path& project, const stdfs::path& source,
                           std::string* error) const {
    if (project.empty()) {
        if (error) *error = "The autosave has no project to belong to.";
        return false;
    }
    const SceneAutosaveSlot slot = SlotFor(project);
    if (!document.SaveAutosave(slot.scene, error)) return false;
    const reflect::Json json{{"$type", "SceneAutosave"},
                             {"version", 1},
                             {"project", project.string()},
                             {"source", source.string()},
                             {"saved", UtcNow()},
                             {"entities", document.Entities().size()}};
    std::string text = json.dump(2);
    text.push_back('\n');
    if (!fs::WriteFileBytes(slot.metadata.string(), text.data(), text.size())) {
        if (error) *error = "The autosave was written but its details (" + slot.metadata.string() + ") were not.";
        return false;
    }
    return true;
}

SceneAutosaveSlot SceneAutosaves::Find(const stdfs::path& project) const {
    SceneAutosaveSlot slot = SlotFor(project);
    std::error_code ec;
    if (!stdfs::is_regular_file(slot.scene, ec)) return {};
    std::vector<u8> bytes;
    if (fs::ReadFileBytes(slot.metadata.string(), bytes)) {
        const reflect::Json json = reflect::Json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
        if (json.is_object()) {
            const auto source = json.find("source");
            if (source != json.end() && source->is_string()) slot.source = stdfs::path(source->get<std::string>());
            const auto saved = json.find("saved");
            if (saved != json.end() && saved->is_string()) slot.saved = saved->get<std::string>();
            const auto entities = json.find("entities");
            if (entities != json.end() && entities->is_number_unsigned()) slot.entities = entities->get<usize>();
        }
    }
    return slot;
}

bool SceneAutosaves::Clear(const stdfs::path& project) const {
    const SceneAutosaveSlot slot = SlotFor(project);
    if (!slot.IsValid()) return false;
    std::error_code ec;
    bool removed = false;
    for (const stdfs::path& file : {slot.scene, stdfs::path(slot.scene.string() + ".bak"), stdfs::path(slot.scene.string() + ".tmp"),
                                    slot.metadata, stdfs::path(slot.metadata.string() + ".tmp")}) {
        removed = stdfs::remove(file, ec) || removed;
    }
    stdfs::remove(slot.scene.parent_path(), ec); // only when the folder is now empty
    return removed;
}

} // namespace aether::editor
