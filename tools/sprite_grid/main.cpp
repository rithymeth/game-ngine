#include "aether/assets/asset_database.h"
#include "aether/assets/image.h"
#include "aether/project/project.h"
#include "aether/sprite2d/atlas.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

using namespace aether;
namespace fs = std::filesystem;

namespace {

int Usage(int code) {
    std::fprintf(code ? stderr : stdout,
        "usage: aether_sprite_grid <project.aproject> <sheet relative to Content> <cell width> <cell height>\n"
        "Creates a sibling .aatlas with numbered frames and a looping grid clip. Never overwrites an atlas.\n");
    return code;
}

int Fail(const std::string& message) {
    std::fprintf(stderr, "aether_sprite_grid: %s\n", message.c_str());
    return 1;
}

bool PositiveInt(std::string_view text, u32& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && value > 0;
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
    if (argc != 5 || !PositiveInt(argv[3], cell_w) || !PositiveInt(argv[4], cell_h)) return Usage(2);

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
    if (image.width % cell_w || image.height % cell_h)
        return Fail("cell dimensions must divide the sheet's " + std::to_string(image.width) + "x" +
                    std::to_string(image.height) + " pixels exactly");
    const u64 count = static_cast<u64>(image.width / cell_w) * (image.height / cell_h);
    if (count == 0 || count > 4096) return Fail("the grid must contain between 1 and 4096 frames");

    assets::AssetDatabase database(content);
    database.Scan();
    const auto* source = database.FindByPath(relative.generic_string());
    if (!source || source->missing || source->importer != "Texture" || source->guid.IsNull())
        return Fail("the asset database could not import the texture; check its .ameta file: " + relative.generic_string());
    auto atlas = sprite2d::AtlasFromGrid(source->guid, image.width, image.height, cell_w, cell_h);
    sprite2d::SpriteClip clip;
    clip.name = "grid";
    for (const auto& frame : atlas.frames) clip.frames.push_back(frame.name);
    atlas.clips.push_back(std::move(clip));

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
    std::printf("Created %s with %zu frames and the looping grid clip (12 fps).\n",
                output.lexically_relative(content).generic_string().c_str(), atlas.frames.size());
    return 0;
}
