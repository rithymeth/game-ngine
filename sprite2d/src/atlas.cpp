#include "aether/sprite2d/atlas.h"
#include "aether/core/json_util.h"

#include "aether/platform/filesystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <unordered_set>

namespace aether::sprite2d {

namespace {
constexpr int kAtlasFormatVersion = 1;
} // namespace

const SpriteFrame* SpriteAtlas::FindFrame(std::string_view name) const {
    for (const SpriteFrame& f : frames) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

const SpriteClip* SpriteAtlas::FindClip(std::string_view name) const {
    for (const SpriteClip& c : clips) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

bool SpriteAtlas::Uv(const SpriteFrame& frame, f32 out[4]) const {
    if (!out || width == 0 || height == 0 || frame.x < 0 || frame.y < 0 || frame.w <= 0 || frame.h <= 0)
        return false;
    const u64 right = static_cast<u64>(frame.x) + static_cast<u64>(frame.w);
    const u64 bottom = static_cast<u64>(frame.y) + static_cast<u64>(frame.h);
    if (right > width || bottom > height) return false;
    out[0] = static_cast<f32>(frame.x) / static_cast<f32>(width);
    out[1] = static_cast<f32>(frame.y) / static_cast<f32>(height);
    out[2] = static_cast<f32>(right) / static_cast<f32>(width);
    out[3] = static_cast<f32>(bottom) / static_cast<f32>(height);
    return true;
}

nlohmann::json AtlasToJson(const SpriteAtlas& atlas) {
    nlohmann::json frames = nlohmann::json::array();
    for (const SpriteFrame& f : atlas.frames) {
        frames.push_back({{"name", f.name}, {"x", f.x}, {"y", f.y}, {"w", f.w}, {"h", f.h},
                          {"pivot", {f.pivot_x, f.pivot_y}}});
    }
    nlohmann::json clips = nlohmann::json::array();
    for (const SpriteClip& c : atlas.clips) {
        clips.push_back({{"name", c.name}, {"frames", c.frames}, {"fps", c.fps}, {"loop", c.loop}});
    }
    return {{"$type", "SpriteAtlas"}, {"$version", kAtlasFormatVersion}, {"texture", assets::ToString(atlas.texture)},
            {"width", atlas.width},   {"height", atlas.height}, {"frames", frames},
            {"clips", clips}};
}

namespace {
// JSON numbers can be wider than the atlas fields. Accept whole pixels only
// and check before converting; double exactly represents every permitted value.
template <class Pixel>
bool ReadPixels(const nlohmann::json& data, const char* key, Pixel& out) {
    const auto entry = data.find(key);
    if (entry == data.end()) { out = 0; return true; }
    if (!entry->is_number()) return false;
    const double n = entry->get<double>();
    if (!std::isfinite(n) || n < 0 || n > std::numeric_limits<Pixel>::max() || std::trunc(n) != n)
        return false;
    out = static_cast<Pixel>(n);
    return true;
}

bool ReadPivotCoordinate(const nlohmann::json& value, f32& out) {
    if (!value.is_number()) return false;
    const f64 n = value.get<f64>();
    if (!std::isfinite(n) || std::abs(n) > std::numeric_limits<f32>::max()) return false;
    out = static_cast<f32>(n);
    return true;
}

bool AtlasFromJsonImpl(const nlohmann::json& data, SpriteAtlas& out, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!data.is_object() || JsonString(data, "$type") != "SpriteAtlas") return fail("not a sprite atlas");
    if (JsonVersion(data) > kAtlasFormatVersion) return fail("saved by a newer version of the engine");
    SpriteAtlas atlas;
    const std::string texture = data.value("texture", "");
    if (!texture.empty() && !assets::ParseAssetGuid(texture, atlas.texture)) return fail("bad texture GUID");
    if (!ReadPixels(data, "width", atlas.width) || !ReadPixels(data, "height", atlas.height))
        return fail("atlas width and height must be non-negative whole pixel counts");
    const auto frames = data.value("frames", nlohmann::json::array());
    // A grid can contain thousands of frames and a clip may reference every one.
    // Index names during asset loading instead of scanning the growing atlas each time.
    std::unordered_set<std::string> frame_names;
    frame_names.reserve(frames.size());
    for (const nlohmann::json& f : frames) {
        SpriteFrame frame;
        frame.name = f.value("name", "");
        if (frame.name.empty()) return fail("a frame has no name");
        if (!frame_names.insert(frame.name).second) return fail("two frames named '" + frame.name + "'");
        if (!ReadPixels(f, "x", frame.x) || !ReadPixels(f, "y", frame.y) ||
            !ReadPixels(f, "w", frame.w) || !ReadPixels(f, "h", frame.h))
            return fail("frame '" + frame.name + "' needs non-negative whole pixel coordinates and size");
        if (frame.w == 0 || frame.h == 0) return fail("frame '" + frame.name + "' has no size");
        f32 uv[4];
        if (!atlas.Uv(frame, uv))
            return fail("frame '" + frame.name + "' is outside the " + std::to_string(atlas.width) +
                        "x" + std::to_string(atlas.height) + " atlas bounds");
        if (const auto pivot = f.find("pivot"); pivot != f.end()) {
            if (!pivot->is_array() || pivot->size() != 2 ||
                !ReadPivotCoordinate((*pivot)[0], frame.pivot_x) || !ReadPivotCoordinate((*pivot)[1], frame.pivot_y))
                return fail("frame '" + frame.name + "' needs two finite pivot coordinates");
        }
        atlas.frames.push_back(std::move(frame));
    }
    const auto clips = data.value("clips", nlohmann::json::array());
    std::unordered_set<std::string> clip_names;
    clip_names.reserve(clips.size());
    for (const nlohmann::json& c : clips) {
        SpriteClip clip;
        clip.name = c.value("name", "");
        if (clip.name.empty()) return fail("a clip has no name");
        if (!clip_names.insert(clip.name).second) return fail("two clips named '" + clip.name + "'");
        clip.fps = c.value("fps", 12.0f);
        clip.loop = c.value("loop", true);
        for (const nlohmann::json& n : c.value("frames", nlohmann::json::array())) {
            const std::string name = n.get<std::string>();
            if (!frame_names.contains(name)) return fail("clip '" + clip.name + "' uses the missing frame '" + name + "'");
            clip.frames.push_back(name);
        }
        atlas.clips.push_back(std::move(clip));
    }
    out = std::move(atlas);
    return true;
}
} // namespace

// A field of the wrong type makes nlohmann throw; a damaged atlas must fail, not end the process.
bool AtlasFromJson(const nlohmann::json& data, SpriteAtlas& out, std::string* error) {
    return GuardJsonLoader(error, "sprite atlas", [&] { return AtlasFromJsonImpl(data, out, error); });
}

bool LoadAtlas(const std::filesystem::path& file, SpriteAtlas& out, std::string* error) {
    std::string text;
    if (!fs::ReadFileText(file.string(), text)) {
        if (error) *error = "Couldn't read " + file.string();
        return false;
    }
    const nlohmann::json data = nlohmann::json::parse(text, nullptr, false);
    if (data.is_discarded()) {
        if (error) *error = file.string() + " isn't valid JSON";
        return false;
    }
    return AtlasFromJson(data, out, error);
}

bool SaveAtlas(const std::filesystem::path& file, const SpriteAtlas& atlas, std::string* error) {
    if (error) error->clear();
    // Encode first so a bad frame or clip name cannot truncate a valid atlas.
    std::string text;
    try {
        text = AtlasToJson(atlas).dump(2);
    } catch (const nlohmann::json::exception& e) {
        if (error) *error = std::string("Couldn't encode sprite atlas: ") + e.what();
        return false;
    }

    if (fs::WriteFileAtomic(file.string(), text.data(), text.size())) return true;
    if (error) *error = "Couldn't write " + file.string();
    return false;
}

usize ClipFrameAt(const SpriteClip& clip, f32 time, bool* finished) {
    if (finished) *finished = false;
    const usize count = clip.frames.size();
    if (count == 0 || !std::isfinite(clip.fps) || clip.fps <= 0.0f || !std::isfinite(time) || time <= 0.0f) return 0;
    const f64 step = std::floor(static_cast<f64>(time) * clip.fps);
    if (clip.loop) return static_cast<usize>(std::fmod(step, static_cast<f64>(count)));
    if (step >= static_cast<f64>(count)) {
        if (finished) *finished = true;
        return count - 1;
    }
    return static_cast<usize>(step);
}

namespace {

u32 NextPow2(u32 v) {
    u32 p = 1;
    while (p < v) {
        if (p > std::numeric_limits<u32>::max() / 2) return 0;
        p <<= 1;
    }
    return p;
}

// Whether `order` (indices, tallest first) shelf-packs into width x height.
bool TryPack(const std::vector<PackInput>& in, const std::vector<usize>& order, u32 padding, u32 width, u32 height,
             std::vector<PackedRect>& rects) {
    rects.assign(in.size(), {});
    u64 x = 0, y = 0, shelf = 0;
    for (usize index : order) {
        const PackInput& r = in[index];
        if (r.w > width || r.h > height) return false;
        if (x + r.w > width) { // next shelf
            y += shelf + padding;
            x = 0;
            shelf = 0;
        }
        if (y + r.h > height) return false;
        rects[index] = {r.name, static_cast<i32>(x), static_cast<i32>(y), r.w, r.h};
        x += r.w + padding;
        shelf = std::max(shelf, static_cast<u64>(r.h));
    }
    return true;
}

} // namespace

bool PackRects(const std::vector<PackInput>& inputs, u32 padding, u32 max_size, PackResult& out, std::string* error) {
    if (inputs.empty()) {
        out = {};
        out.width = out.height = 1;
        return true;
    }
    std::vector<usize> order(inputs.size());
    std::iota(order.begin(), order.end(), usize{0});
    std::stable_sort(order.begin(), order.end(), [&](usize a, usize b) { return inputs[a].h > inputs[b].h; });
    u64 area = 0;
    u32 widest = 1, tallest = 1;
    if (max_size == 0 || max_size > static_cast<u32>(std::numeric_limits<i32>::max()) || padding > max_size) {
        if (error) *error = "the sprites don't fit in " + std::to_string(max_size) + "x" + std::to_string(max_size);
        return false;
    }
    const u64 max_area = static_cast<u64>(max_size) * max_size;
    for (const PackInput& r : inputs) {
        if (r.w == 0 || r.h == 0) {
            if (error) *error = "'" + r.name + "' has no size";
            return false;
        }
        if (r.w > static_cast<u32>(std::numeric_limits<i32>::max()) ||
            r.h > static_cast<u32>(std::numeric_limits<i32>::max()) ||
            static_cast<u64>(r.w) + padding > max_size || static_cast<u64>(r.h) + padding > max_size) {
            if (error) *error = "the sprites don't fit in " + std::to_string(max_size) + "x" + std::to_string(max_size);
            return false;
        }
        const u64 padded_area = (static_cast<u64>(r.w) + padding) * (static_cast<u64>(r.h) + padding);
        if (padded_area > max_area - area) {
            if (error) *error = "the sprites don't fit in " + std::to_string(max_size) + "x" + std::to_string(max_size);
            return false;
        }
        area += padded_area;
        widest = std::max(widest, r.w);
        tallest = std::max(tallest, r.h);
    }
    // Grow the texture, trying a wide shape before a taller one, until it fits.
    u32 side = NextPow2(static_cast<u32>(std::ceil(std::sqrt(static_cast<f64>(area)))));
    side = std::max({side, NextPow2(widest), 1u});
    for (; side != 0 && side <= max_size;) {
        for (u32 height : {side / 2, side}) {
            if (height == 0 || height < tallest) continue;
            std::vector<PackedRect> rects;
            if (TryPack(inputs, order, padding, side, height, rects)) {
                out.width = side;
                out.height = height;
                out.rects = std::move(rects);
                return true;
            }
        }
        if (side > max_size / 2) break;
        side <<= 1;
    }
    if (error) *error = "the sprites don't fit in " + std::to_string(max_size) + "x" + std::to_string(max_size);
    return false;
}

bool ComposeAtlasImage(const std::vector<assets::ImageData>& images, const PackResult& packed, assets::ImageData& out,
                       std::string* error) {
    if (images.size() != packed.rects.size()) {
        if (error) *error = "the images and the packing differ in count";
        return false;
    }
    assets::ImageData atlas;
    atlas.width = packed.width;
    atlas.height = packed.height;
    atlas.pixels.assign(static_cast<usize>(packed.width) * packed.height * 4, 0);
    for (usize i = 0; i < images.size(); ++i) {
        const assets::ImageData& img = images[i];
        const PackedRect& r = packed.rects[i];
        if (img.width != r.w || img.height != r.h || img.pixels.size() != static_cast<usize>(r.w) * r.h * 4 ||
            r.x < 0 || r.y < 0 || static_cast<u32>(r.x) + r.w > atlas.width || static_cast<u32>(r.y) + r.h > atlas.height) {
            if (error) *error = "'" + r.name + "' doesn't match its packed rectangle";
            return false;
        }
        for (u32 row = 0; row < r.h; ++row) {
            std::memcpy(&atlas.pixels[((static_cast<usize>(r.y) + row) * atlas.width + static_cast<u32>(r.x)) * 4],
                        &img.pixels[static_cast<usize>(row) * r.w * 4], static_cast<usize>(r.w) * 4);
        }
    }
    out = std::move(atlas);
    return true;
}

SpriteAtlas AtlasFromPack(const assets::AssetGuid& texture, const PackResult& packed) {
    SpriteAtlas atlas;
    atlas.texture = texture;
    atlas.width = packed.width;
    atlas.height = packed.height;
    for (const PackedRect& r : packed.rects) {
        SpriteFrame f;
        f.name = r.name;
        f.x = r.x;
        f.y = r.y;
        f.w = static_cast<i32>(r.w);
        f.h = static_cast<i32>(r.h);
        atlas.frames.push_back(std::move(f));
    }
    return atlas;
}

SpriteAtlas AtlasFromGrid(const assets::AssetGuid& texture, u32 width, u32 height, u32 cell_w, u32 cell_h,
                          const std::string& prefix) {
    SpriteAtlas atlas;
    atlas.texture = texture;
    atlas.width = width;
    atlas.height = height;
    if (cell_w == 0 || cell_h == 0) return atlas;
    usize n = 0;
    for (u32 y = 0; y + cell_h <= height; y += cell_h) {
        for (u32 x = 0; x + cell_w <= width; x += cell_w) {
            SpriteFrame f;
            f.name = prefix + "_" + std::to_string(n++);
            f.x = static_cast<i32>(x);
            f.y = static_cast<i32>(y);
            f.w = static_cast<i32>(cell_w);
            f.h = static_cast<i32>(cell_h);
            atlas.frames.push_back(std::move(f));
        }
    }
    return atlas;
}

} // namespace aether::sprite2d
