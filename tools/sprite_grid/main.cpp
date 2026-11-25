#include "aether/assets/asset_database.h"
#include "aether/assets/image.h"
#include "aether/project/project.h"
#include "aether/sprite2d/atlas.h"
#include "aether/sprite2d/components.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace aether;
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

struct ClipRange {
    std::string name;
    u32 first = 0, count = 0, fps = 0;
    bool loop = true;
};

bool ClipName(std::string_view name) {
    return !name.empty() && name.size() <= kMaxClipName && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

u32 GridCells(u32 size, u32 cell, u32 margin, u32 spacing) {
    const u64 border = 2ull * margin;
    if (size < border + cell) return 0;
    const u64 available = static_cast<u64>(size) - border + spacing;
    const u64 stride = static_cast<u64>(cell) + spacing;
    return available % stride == 0 ? static_cast<u32>(available / stride) : 0;
}

bool VisibleRelative(const fs::path& path) {
    if (path.has_root_path() || path.empty()) return false;
    for (const auto& part : path) {
        const auto& name = part.native();
        if (name.empty() || name.front() == '.') return false;
    }
    return true;
}

bool Occupied(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    return status.type() != fs::file_type::not_found; // uncertain paths and dangling links are not safe outputs
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

    const fs::path requested(argv[2]);
    if (!VisibleRelative(requested)) return Fail("choose a visible sprite sheet inside Content");
    std::error_code ec;
    const auto content = fs::canonical(ProjectPaths::ForFile(project_file).content, ec);
    if (ec) return Fail("cannot open the project's Content folder");
    const auto sheet = fs::canonical(content / requested, ec);
    if (ec || !fs::is_regular_file(sheet, ec)) return Fail("cannot find the sprite sheet inside Content: " + requested.string());
    const auto relative = sheet.lexically_relative(content);
    if (!VisibleRelative(relative)) return Fail("choose a visible sprite sheet inside Content");
    auto extension = sheet.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (assets::ImporterForExtension(sheet) != "Texture" || extension == ".hdr")
        return Fail("choose a supported sprite-sheet texture (PNG, JPEG, BMP or TGA)");

    auto output = sheet;
    output.replace_extension(".aatlas");
    auto output_meta = output;
    output_meta += assets::AssetMeta::kExtension;
    if (Occupied(output) || Occupied(output_meta)) return Fail("atlas or its metadata already exists: " + output.string());

    assets::ImageData image;
    constexpr u64 kMaxPixels = 16 * 1024 * 1024;
    if (!assets::DecodeImageFile(sheet.string(), image, kMaxPixels))
        return Fail("cannot decode the sprite sheet as an image of up to 16 million pixels: " + requested.string());
    const u32 cols = GridCells(image.width, cell_w, margin_x, spacing_x);
    const u32 rows = GridCells(image.height, cell_h, margin_y, spacing_y);
    if (!cols || !rows)
        return Fail(margin_set || spacing_set ? "margin, spacing and cell size must leave only complete cells"
            : "cell dimensions must divide the sheet's " + std::to_string(image.width) + "x" +
                  std::to_string(image.height) + " pixels exactly");
    const u64 count = static_cast<u64>(cols) * rows;
    if (count == 0 || count > 4096) return Fail("the grid must contain between 1 and 4096 frames");
    for (const auto& range : ranges) {
        if (range.first >= count || range.count > count - range.first)
            return Fail("clip '" + range.name + "' goes outside the " + std::to_string(count) + "-frame grid");
    }
    if (ranges.empty()) ranges.push_back({"grid", 0, static_cast<u32>(count), 12});

    assets::AssetDatabase database(content);
    database.Scan();
    const auto* source = database.FindByPath(relative.generic_string());
    if (!source || source->missing || source->importer != "Texture" || source->guid.IsNull())
        return Fail("the asset database could not import the texture; check its .ameta file: " + relative.generic_string());
    auto atlas = sprite2d::AtlasFromGrid(source->guid, cols * cell_w, rows * cell_h, cell_w, cell_h);
    atlas.width = image.width;
    atlas.height = image.height;
    for (auto& frame : atlas.frames) {
        frame.x = static_cast<i32>(margin_x + static_cast<u64>(frame.x / cell_w) * (static_cast<u64>(cell_w) + spacing_x));
        frame.y = static_cast<i32>(margin_y + static_cast<u64>(frame.y / cell_h) * (static_cast<u64>(cell_h) + spacing_y));
    }
    for (const auto& range : ranges) {
        sprite2d::SpriteClip clip;
        clip.name = range.name;
        clip.fps = static_cast<f32>(range.fps);
        clip.loop = range.loop;
        clip.frames.reserve(range.count);
        for (u32 index = 0; index < range.count; ++index)
            clip.frames.push_back(atlas.frames[range.first + index].name);
        atlas.clips.push_back(std::move(clip));
    }

    const std::string json = sprite2d::AtlasToJson(atlas).dump(2) + "\n";
    std::FILE* file = std::fopen(output.string().c_str(), "wbx");
    if (!file) return Fail(errno == EEXIST ? "atlas already exists: " + output.string()
                                          : "cannot create atlas: " + output.string());
    const bool wrote = std::fwrite(json.data(), 1, json.size(), file) == json.size();
    const bool closed = std::fclose(file) == 0;
    if (!wrote || !closed) {
        fs::remove(output, ec);
        return Fail("could not finish writing atlas: " + output.string());
    }
    std::printf("Created %s with %zu frames and %zu clip(s).\n",
                output.lexically_relative(content).generic_string().c_str(),
                atlas.frames.size(), atlas.clips.size());
    return 0;
}
