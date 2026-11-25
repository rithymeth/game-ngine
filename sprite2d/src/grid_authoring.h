#pragma once

#include "aether/core/base.h"

#include <filesystem>
#include <string>
#include <vector>

namespace aether::sprite2d::authoring {

struct ClipRange {
    std::string name;
    u32 first = 0, count = 0, fps = 0;
    bool loop = true;
};

struct Grid {
    u32 cell_w = 0, cell_h = 0;
    u32 margin_x = 0, margin_y = 0, spacing_x = 0, spacing_y = 0;
};

struct CreatedAtlas {
    std::string relative_path;
    usize frames = 0, clips = 0;
};

// Tool-internal: create a new sibling atlas. Clip ranges arrive from
// validated tools; an empty vector generates the default 12 fps grid clip.
bool CreateGridAtlas(const std::filesystem::path& content_root, const std::filesystem::path& requested,
                     const Grid& grid, std::vector<ClipRange> ranges, CreatedAtlas& result, std::string* error);

} // namespace aether::sprite2d::authoring
