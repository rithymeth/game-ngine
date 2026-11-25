#include "aether/project/project.h"
#include "grid_authoring.h"
#include "aether/sprite2d/components.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace aether;
using sprite2d::authoring::ClipRange;
namespace fs = std::filesystem;

namespace {

int Usage(int code) {
    std::fprintf(code ? stderr : stdout,
        "usage: aether_sprite_grid <project.aproject> <sheet relative to Content> <cell width> <cell height>\n"
        "       [--margin X Y] [--spacing X Y]\n"
        "       [--clip NAME FIRST COUNT FPS | --clip-once NAME FIRST COUNT FPS]...\n"
        "FIRST is zero-based. --clip loops; --clip-once holds its final frame.\n"
        "Without clip options, the whole sheet loops at 12 fps.\n"
        "Creates a sibling .aatlas without overwriting an existing atlas.\n");
    return code;
}

int Fail(const std::string& message, int code = 1) {
    std::fprintf(stderr, "aether_sprite_grid: %s\n", message.c_str());
    return code;
}

bool UnsignedInt(std::string_view text, u32& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

constexpr usize kMaxClipName = sizeof(sprite2d::SpriteAnimator::clip) - 1;

bool ClipName(std::string_view name) {
    return !name.empty() && name.size() <= kMaxClipName && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") return Usage(0);
    u32 cell_w = 0, cell_h = 0;
    if (argc < 5 || !UnsignedInt(argv[3], cell_w) || !UnsignedInt(argv[4], cell_h) ||
        cell_w == 0 || cell_h == 0) return Usage(2);

    std::vector<ClipRange> ranges;
    u32 margin_x = 0, margin_y = 0, spacing_x = 0, spacing_y = 0;
    bool margin_set = false, spacing_set = false;
    for (int i = 5; i < argc;) {
        const std::string_view option = argv[i];
        if (option == "--margin" || option == "--spacing") {
            const bool margin = option == "--margin";
            bool& set = margin ? margin_set : spacing_set;
            u32& x = margin ? margin_x : spacing_x;
            u32& y = margin ? margin_y : spacing_y;
            if (set) return Fail(std::string(option) + " may be given only once", 2);
            if (argc - i < 3 || !UnsignedInt(argv[i + 1], x) || !UnsignedInt(argv[i + 2], y))
                return Fail(std::string(option) + " needs non-negative X and Y pixel counts", 2);
            set = true;
            i += 3;
            continue;
        }
        if ((option != "--clip" && option != "--clip-once") || argc - i < 5) return Usage(2);
        if (ranges.size() >= 64) return Fail("use at most 64 named clips", 2);
        ClipRange range;
        range.name = argv[i + 1];
        range.loop = option == "--clip";
        if (!ClipName(range.name))
            return Fail("clip names must be 1 to " + std::to_string(kMaxClipName) +
                        " ASCII letters, digits, underscores or hyphens", 2);
        if (std::any_of(ranges.begin(), ranges.end(), [&](const ClipRange& old) { return old.name == range.name; }))
            return Fail("duplicate clip name '" + range.name + "'", 2);
        if (!UnsignedInt(argv[i + 2], range.first) || !UnsignedInt(argv[i + 3], range.count) || range.count == 0)
            return Fail("clip '" + range.name + "' requires a zero-based FIRST and positive COUNT", 2);
        if (!UnsignedInt(argv[i + 4], range.fps) || range.fps == 0 || range.fps > 240)
            return Fail("clip '" + range.name + "' FPS must be an integer from 1 to 240", 2);
        ranges.push_back(std::move(range));
        i += 5;
    }

    const fs::path project_file(argv[1]);
    ProjectSettings settings;
    std::string error;
    if (!LoadProject(project_file, settings, &error)) return Fail(error);

    sprite2d::authoring::CreatedAtlas result;
    if (!sprite2d::authoring::CreateGridAtlas(ProjectPaths::ForFile(project_file).content, fs::path(argv[2]),
            {cell_w, cell_h, margin_x, margin_y, spacing_x, spacing_y}, std::move(ranges), result, &error))
        return Fail(error);
    std::printf("Created %s with %zu frames and %zu clip(s).\n", result.relative_path.c_str(), result.frames, result.clips);
    return 0;
}
