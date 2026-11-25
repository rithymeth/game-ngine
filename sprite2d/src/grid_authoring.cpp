#include "grid_authoring.h"

#include "aether/assets/asset_database.h"
#include "aether/assets/image.h"
#include "aether/sprite2d/atlas.h"
#include "aether/sprite2d/components.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>

namespace aether::sprite2d::authoring {
namespace fs = std::filesystem;
namespace {
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

std::string ClipRangesError(const std::vector<ClipRange>& ranges) {
    constexpr usize name_limit = sizeof(SpriteAnimator::clip) - 1;
    if (ranges.size() > kMaxGridClips)
        return "use at most " + std::to_string(kMaxGridClips) + " named clips";
    for (auto it = ranges.begin(); it != ranges.end(); ++it) {
        const auto& range = *it;
        if (range.name.empty() || range.name.size() > name_limit ||
            !std::all_of(range.name.begin(), range.name.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_' || c == '-';
            }))
            return "clip names must be 1 to " + std::to_string(name_limit) + " ASCII letters, digits, underscores or hyphens";
        if (std::any_of(ranges.begin(), it, [&](const ClipRange& prior) { return prior.name == range.name; }))
            return "duplicate clip name '" + range.name + "'";
        if (!range.count) return "clip '" + range.name + "' requires a zero-based FIRST and positive COUNT";
        if (!range.fps || range.fps > 240) return "clip '" + range.name + "' FPS must be an integer from 1 to 240";
    }
    return {};
}

bool CreateGridAtlas(const fs::path& content_root, const fs::path& requested, const Grid& grid,
                     std::vector<ClipRange> ranges, CreatedAtlas& result, std::string* error) {
    if (error) error->clear();
    const auto Fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    const auto [cell_w, cell_h, margin_x, margin_y, spacing_x, spacing_y] = grid;
    if (!cell_w || !cell_h) return Fail("cell width and height must be positive");
    if (const std::string why = ClipRangesError(ranges); !why.empty()) return Fail(why);
    if (!VisibleRelative(requested)) return Fail("choose a visible sprite sheet inside Content");
    std::error_code ec;
    const auto content = fs::canonical(content_root, ec);
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
        return Fail((margin_x || margin_y || spacing_x || spacing_y) ? "margin, spacing and cell size must leave only complete cells"
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
    auto atlas = AtlasFromGrid(source->guid, cols * cell_w, rows * cell_h, cell_w, cell_h);
    atlas.width = image.width;
    atlas.height = image.height;
    for (auto& frame : atlas.frames) {
        frame.x = static_cast<i32>(margin_x + static_cast<u64>(frame.x / cell_w) * (static_cast<u64>(cell_w) + spacing_x));
        frame.y = static_cast<i32>(margin_y + static_cast<u64>(frame.y / cell_h) * (static_cast<u64>(cell_h) + spacing_y));
    }
    for (const auto& range : ranges) {
        SpriteClip clip;
        clip.name = range.name;
        clip.fps = static_cast<f32>(range.fps);
        clip.loop = range.loop;
        clip.frames.reserve(range.count);
        for (u32 index = 0; index < range.count; ++index)
            clip.frames.push_back(atlas.frames[range.first + index].name);
        atlas.clips.push_back(std::move(clip));
    }

    const std::string json = AtlasToJson(atlas).dump(2) + "\n";
    std::FILE* file = std::fopen(output.string().c_str(), "wbx");
    if (!file) return Fail(errno == EEXIST ? "atlas already exists: " + output.string()
                                          : "cannot create atlas: " + output.string());
    const bool wrote = std::fwrite(json.data(), 1, json.size(), file) == json.size();
    const bool closed = std::fclose(file) == 0;
    if (!wrote || !closed) {
        fs::remove(output, ec);
        return Fail("could not finish writing atlas: " + output.string());
    }
    result = {output.lexically_relative(content).generic_string(), atlas.frames.size(), atlas.clips.size()};
    return true;
}

} // namespace aether::sprite2d::authoring
