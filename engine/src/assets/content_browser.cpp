#include "aether/assets/content_browser.h"

#include "aether/assets/gltf_references.h"
#include "aether/assets/model_importer.h"
#include "aether/assets/texture_importer.h"
#include "aether/reflection/serialize.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <system_error>
#include <unordered_set>

namespace aether::assets {

namespace stdfs = std::filesystem;

namespace {

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

std::string ParentFolder(const std::string& path) { return stdfs::path(path).parent_path().generic_string(); }

bool IsInside(const std::string& path, const std::string& folder) {
    return folder.empty() || path == folder || (path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 &&
                                                path[folder.size()] == '/');
}

std::string Normalized(const std::string& path) {
    std::string text = stdfs::path(path).lexically_normal().generic_string();
    while (!text.empty() && text.back() == '/') {
        text.pop_back();
    }
    return text == "." ? std::string() : text;
}

// Stays inside the content root: relative, no "..".
bool IsContentPath(const std::string& normalized) {
    return !normalized.empty() && !stdfs::path(normalized).is_absolute() && normalized.rfind("..", 0) != 0;
}

MoveResult Fail(std::string message) {
    MoveResult result;
    result.error = std::move(message);
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Folders and listing
// ---------------------------------------------------------------------------

std::vector<std::string> ContentFolders(const AssetDatabase& database) {
    std::vector<std::string> folders;
    const stdfs::path& root = database.ContentRoot();
    std::error_code ec;
    for (auto it = stdfs::recursive_directory_iterator(root, ec); !ec && it != stdfs::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code entry_ec;
        if (!it->is_directory(entry_ec)) {
            continue;
        }
        const std::string name = it->path().filename().string();
        if (!name.empty() && name.front() == '.') {
            it.disable_recursion_pending();
            continue;
        }
        folders.push_back(stdfs::relative(it->path(), root, entry_ec).generic_string());
    }
    std::sort(folders.begin(), folders.end());
    return folders;
}

std::vector<ContentEntry> ListContent(const AssetDatabase& database, const ContentQuery& query) {
    const std::string folder = Normalized(query.folder);
    const std::string search = Lower(query.search);
    const bool recursive = query.recursive || !search.empty();
    std::vector<ContentEntry> folders;
    std::vector<ContentEntry> assets;

    auto matches = [&](const std::string& name) { return search.empty() || Lower(name).find(search) != std::string::npos; };

    if (!recursive) {
        for (const std::string& path : ContentFolders(database)) {
            if (ParentFolder(path) == folder) {
                folders.push_back({true, stdfs::path(path).filename().string(), path, nullptr});
            }
        }
    }
    for (const AssetRecord* record : database.All()) {
        if (record->IsSubAsset() && !query.include_sub_assets) {
            continue;
        }
        const AssetRecord* source = record->IsSubAsset() ? database.Find(record->parent) : record;
        if (source == nullptr) {
            continue;
        }
        const std::string record_folder = ParentFolder(source->path);
        if (recursive ? !IsInside(record_folder, folder) : record_folder != folder) {
            continue;
        }
        if (!query.types.empty() &&
            std::find(query.types.begin(), query.types.end(), record->importer) == query.types.end()) {
            continue;
        }
        std::string name = record->IsSubAsset() ? record->sub_key : stdfs::path(record->path).filename().string();
        if (!matches(name)) {
            continue;
        }
        assets.push_back({false, std::move(name), record->path, record});
    }
    auto by_name = [](const ContentEntry& a, const ContentEntry& b) {
        const std::string la = Lower(a.name), lb = Lower(b.name);
        return la != lb ? la < lb : a.path < b.path;
    };
    std::sort(folders.begin(), folders.end(), by_name);
    std::sort(assets.begin(), assets.end(), by_name);
    folders.insert(folders.end(), std::make_move_iterator(assets.begin()), std::make_move_iterator(assets.end()));
    return folders;
}

// ---------------------------------------------------------------------------
// Rename, move, create
// ---------------------------------------------------------------------------

bool IsValidAssetName(const std::string& name, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (name.empty()) {
        return fail("The name can't be empty");
    }
    for (unsigned char c : name) {
        if (c < 32 || std::string_view("<>:\"/\\|?*#").find(static_cast<char>(c)) != std::string_view::npos) {
            return fail("The name can't contain < > : \" / \\ | ? * # or control characters");
        }
    }
    if (name.front() == '.') {
        return fail("The name can't start with '.' (hidden files are ignored)");
    }
    if (name.back() == '.' || name.back() == ' ') {
        return fail("The name can't end with '.' or a space");
    }
    std::string stem = Lower(name.substr(0, name.find('.')));
    static const std::set<std::string> kReserved = {"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
                                                    "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
                                                    "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    if (kReserved.count(stem) != 0) {
        return fail("\"" + name + "\" is a reserved name on Windows");
    }
    return true;
}

MoveResult MoveContent(AssetDatabase& database, const std::string& from_path, const std::string& to_path) {
    const std::string from = Normalized(from_path);
    const std::string to = Normalized(to_path);
    const stdfs::path& root = database.ContentRoot();
    if (!IsContentPath(from) || !IsContentPath(to)) {
        return Fail("Both paths must be inside the content folder");
    }
    if (from.find('#') != std::string::npos) {
        return Fail(from + " is part of another asset; move its source instead");
    }
    if (Lower(stdfs::path(from).extension().string()) == AssetMeta::kExtension ||
        Lower(stdfs::path(to).extension().string()) == AssetMeta::kExtension) {
        return Fail(".ameta files move with their asset");
    }
    for (const stdfs::path& part : stdfs::path(to)) {
        std::string error;
        if (!IsValidAssetName(part.string(), &error)) {
            return Fail(error);
        }
    }
    if (IsInside(to, from)) {
        return Fail("Can't move " + from + " into itself");
    }
    std::error_code ec;
    const stdfs::path from_abs = root / from;
    const stdfs::path to_abs = root / to;
    const bool is_folder = stdfs::is_directory(from_abs, ec);
    if (!is_folder && !stdfs::is_regular_file(from_abs, ec)) {
        return Fail(from + " doesn't exist");
    }
    stdfs::path from_meta = from_abs;
    from_meta += AssetMeta::kExtension;
    stdfs::path to_meta = to_abs;
    to_meta += AssetMeta::kExtension;
    if (stdfs::exists(to_abs, ec) || (!is_folder && stdfs::exists(to_meta, ec))) {
        return Fail(to + " already exists");
    }

    // Relative references from every .gltf, taken before anything moves.
    struct GltfRefs {
        std::string path;
        std::vector<GltfFileReference> refs;
    };
    std::vector<GltfRefs> gltfs;
    for (const AssetRecord* record : database.All()) {
        if (!record->IsSubAsset() && !record->missing && record->importer == "Model") {
            std::vector<GltfFileReference> refs = GltfFileReferences(root, record->path);
            if (!refs.empty()) {
                gltfs.push_back({record->path, std::move(refs)});
            }
        }
    }

    // Move: a folder in one go (with every .ameta and helper file in it), a
    // file together with its .ameta.
    stdfs::create_directories(to_abs.parent_path(), ec);
    stdfs::rename(from_abs, to_abs, ec);
    if (ec) {
        return Fail("Couldn't move " + from + ": " + ec.message());
    }
    if (!is_folder && stdfs::exists(from_meta)) {
        stdfs::rename(from_meta, to_meta, ec);
        if (ec) {
            std::error_code undo;
            stdfs::rename(to_abs, from_abs, undo); // keep the pair together
            return Fail("Couldn't move the .ameta of " + from + ": " + ec.message());
        }
    }

    // Where a content path is after the move.
    auto moved = [&](const std::string& path) {
        return IsInside(path, from) ? to + path.substr(from.size()) : path;
    };

    MoveResult result;
    result.ok = true;
    for (const GltfRefs& gltf : gltfs) {
        const std::string new_gltf = moved(gltf.path);
        const stdfs::path old_folder = (root / gltf.path).parent_path();
        const stdfs::path new_folder = (root / new_gltf).parent_path();
        std::map<std::string, std::string> new_uris;
        for (const GltfFileReference& ref : gltf.refs) {
            // Work on absolute paths so references outside Content/ are kept too.
            stdfs::path target = (old_folder / UriDecodePath(ref.uri)).lexically_normal();
            if (!ref.path.empty()) {
                target = root / moved(ref.path);
            }
            if (new_gltf == gltf.path && (ref.path.empty() || moved(ref.path) == ref.path)) {
                continue; // neither end moved
            }
            new_uris[ref.uri] = UriEncodePath(target.lexically_normal().lexically_relative(new_folder).generic_string());
        }
        if (new_uris.empty()) {
            continue;
        }
        std::string error;
        const int changed = RewriteGltfUris(
            root / new_gltf,
            [&](const std::string& uri) -> std::optional<std::string> {
                auto it = new_uris.find(uri);
                return it != new_uris.end() ? std::optional<std::string>(it->second) : std::nullopt;
            },
            &error);
        if (changed < 0) {
            result.error += (result.error.empty() ? "" : "; ") + error; // moved, but this file needs fixing by hand
        } else if (changed > 0) {
            result.rewritten.push_back(new_gltf);
        }
    }
    std::sort(result.rewritten.begin(), result.rewritten.end());
    database.Scan();
    return result;
}

MoveResult RenameContent(AssetDatabase& database, const std::string& path, const std::string& new_name) {
    std::string error;
    if (!IsValidAssetName(new_name, &error)) {
        return Fail(error);
    }
    const std::string from = Normalized(path);
    std::error_code ec;
    std::string name = new_name;
    if (!stdfs::is_directory(database.ContentRoot() / from, ec)) {
        const std::string old_ext = stdfs::path(from).extension().string();
        const std::string new_ext = stdfs::path(name).extension().string();
        if (new_ext.empty()) {
            name += old_ext;
        } else if (Lower(new_ext) != Lower(old_ext)) {
            return Fail("Changing the extension (" + old_ext + " to " + new_ext + ") would change how it's imported");
        }
    }
    const std::string parent = ParentFolder(from);
    return MoveContent(database, from, parent.empty() ? name : parent + "/" + name);
}

bool CreateContentFolder(AssetDatabase& database, const std::string& parent, const std::string& name,
                         std::string* error) {
    if (!IsValidAssetName(name, error)) {
        return false;
    }
    const std::string folder = Normalized(parent);
    if (!folder.empty() && !IsContentPath(folder)) {
        if (error != nullptr) {
            *error = "The folder must be inside the content folder";
        }
        return false;
    }
    const stdfs::path target = database.ContentRoot() / folder / name;
    std::error_code ec;
    if (stdfs::exists(target, ec)) {
        if (error != nullptr) {
            *error = (folder.empty() ? name : folder + "/" + name) + " already exists";
        }
        return false;
    }
    if (!stdfs::create_directories(target, ec)) {
        if (error != nullptr) {
            *error = "Couldn't create the folder: " + ec.message();
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Reference Viewer
// ---------------------------------------------------------------------------

namespace {

std::vector<AssetGuid> Neighbours(const AssetDatabase& database, const AssetRecord& record, ReferenceDirection direction) {
    std::vector<AssetGuid> result;
    if (direction == ReferenceDirection::Dependencies) {
        result = record.dependencies;
    } else {
        auto add = [&](const AssetGuid& used) {
            for (const AssetRecord* user : database.Referencers(used)) {
                if (std::find(result.begin(), result.end(), user->guid) == result.end()) {
                    result.push_back(user->guid);
                }
            }
        };
        add(record.guid);
        for (const AssetGuid& sub : record.sub_assets) {
            add(sub);
        }
    }
    std::sort(result.begin(), result.end(), [&](const AssetGuid& a, const AssetGuid& b) {
        const AssetRecord* ra = database.Find(a);
        const AssetRecord* rb = database.Find(b);
        return (ra ? ra->path : std::string()) < (rb ? rb->path : std::string());
    });
    return result;
}

void Visit(const AssetDatabase& database, const AssetGuid& guid, ReferenceDirection direction, u32 depth,
           u32 max_depth, std::unordered_set<AssetGuid>& seen, std::vector<ReferenceNode>& out) {
    const AssetRecord* record = database.Find(guid);
    if (record == nullptr) {
        return;
    }
    ReferenceNode node{guid, record->path, depth, seen.count(guid) != 0};
    out.push_back(node);
    if (node.repeated || depth >= max_depth) {
        return;
    }
    seen.insert(guid);
    for (const AssetGuid& next : Neighbours(database, *record, direction)) {
        Visit(database, next, direction, depth + 1, max_depth, seen, out);
    }
}

} // namespace

std::vector<ReferenceNode> CollectReferences(const AssetDatabase& database, const AssetGuid& guid,
                                             ReferenceDirection direction, u32 max_depth) {
    std::vector<ReferenceNode> nodes;
    std::unordered_set<AssetGuid> seen;
    Visit(database, guid, direction, 0, max_depth, seen, nodes);
    return nodes;
}

// ---------------------------------------------------------------------------
// Thumbnails
// ---------------------------------------------------------------------------

Thumbnail MakeThumbnail(const std::vector<u8>& rgba, u32 width, u32 height, u32 max_size) {
    Thumbnail thumb;
    if (width == 0 || height == 0 || max_size == 0 || rgba.size() < static_cast<usize>(width) * height * 4) {
        return thumb;
    }
    const u32 longest = std::max(width, height);
    const u32 out_w = longest <= max_size ? width : std::max<u32>(1, static_cast<u32>(u64{width} * max_size / longest));
    const u32 out_h = longest <= max_size ? height : std::max<u32>(1, static_cast<u32>(u64{height} * max_size / longest));
    thumb.width = out_w;
    thumb.height = out_h;
    thumb.rgba.resize(static_cast<usize>(out_w) * out_h * 4);
    for (u32 y = 0; y < out_h; ++y) {
        const u32 y0 = static_cast<u32>(u64{y} * height / out_h);
        const u32 y1 = std::max(y0 + 1, static_cast<u32>(u64{y + 1} * height / out_h));
        for (u32 x = 0; x < out_w; ++x) {
            const u32 x0 = static_cast<u32>(u64{x} * width / out_w);
            const u32 x1 = std::max(x0 + 1, static_cast<u32>(u64{x + 1} * width / out_w));
            u32 sum[4] = {0, 0, 0, 0};
            for (u32 sy = y0; sy < y1; ++sy) {
                for (u32 sx = x0; sx < x1; ++sx) {
                    const u8* p = &rgba[(static_cast<usize>(sy) * width + sx) * 4];
                    for (int c = 0; c < 4; ++c) {
                        sum[c] += p[c];
                    }
                }
            }
            const u32 count = (y1 - y0) * (x1 - x0);
            u8* out = &thumb.rgba[(static_cast<usize>(y) * out_w + x) * 4];
            for (int c = 0; c < 4; ++c) {
                out[c] = static_cast<u8>((sum[c] + count / 2) / count);
            }
        }
    }
    return thumb;
}

namespace {

f32 SrgbToLinear(f32 c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
u8 LinearToSrgb8(f32 c) {
    c = std::clamp(c, 0.0f, 1.0f);
    const f32 s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<u8>(std::lround(s * 255.0f));
}

std::optional<Thumbnail> TextureThumbnail(AssetDatabase& database, const AssetGuid& guid,
                                          const ImporterRegistry& importers, DerivedDataCache& cache, u32 max_size) {
    ImportOutput imported = ImportAsset(database, guid, importers, cache);
    TextureData texture;
    if (!imported.ok || !DecodeTextureData(imported.data, texture) || texture.mips.empty()) {
        return std::nullopt;
    }
    // Start from the smallest mip that's still at least the thumbnail size.
    usize level = 0;
    while (level + 1 < texture.mips.size() &&
           std::max(texture.MipWidth(level + 1), texture.MipHeight(level + 1)) >= max_size) {
        ++level;
    }
    return MakeThumbnail(texture.mips[level], texture.MipWidth(level), texture.MipHeight(level), max_size);
}

} // namespace

std::optional<Thumbnail> AssetThumbnail(AssetDatabase& database, const AssetGuid& guid,
                                        const ImporterRegistry& importers, DerivedDataCache& cache, u32 max_size) {
    const AssetRecord* record = database.Find(guid);
    if (record == nullptr || record->missing || max_size == 0) {
        return std::nullopt;
    }
    if (record->importer == "Texture") {
        return TextureThumbnail(database, guid, importers, cache, max_size);
    }
    if (record->importer != "Material") {
        return std::nullopt;
    }
    ImportOutput imported = ImportAsset(database, guid, importers, cache);
    MaterialData material;
    if (!imported.ok || !reflect::LoadBinary(material, imported.data)) {
        return std::nullopt;
    }
    const f32 factor[4] = {material.base_color.x, material.base_color.y, material.base_color.z, material.base_color.w};
    std::optional<Thumbnail> thumb;
    if (const AssetRecord* texture = database.FindByPath(material.base_color_texture);
        texture != nullptr && texture->importer == "Texture") {
        thumb = TextureThumbnail(database, texture->guid, importers, cache, max_size);
    }
    if (!thumb) {
        // A plain swatch of the colour.
        thumb = Thumbnail{max_size, max_size, std::vector<u8>(static_cast<usize>(max_size) * max_size * 4, 255)};
    }
    // Tint in linear light (the texture is sRGB; the factor is linear).
    for (usize i = 0; i < thumb->rgba.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            thumb->rgba[i + c] = LinearToSrgb8(SrgbToLinear(thumb->rgba[i + c] / 255.0f) * factor[c]);
        }
        thumb->rgba[i + 3] = static_cast<u8>(std::lround(std::clamp(thumb->rgba[i + 3] * factor[3], 0.0f, 255.0f)));
    }
    return thumb;
}

} // namespace aether::assets
