#pragma once

#include "aether/core/base.h"
#include "aether/math/mat4.h"

#include <array>
#include <span>
#include <vector>

namespace aether {
class JobSystem;
}

namespace aether::assets {

// A vertex as the player uploads it (48 bytes). `uv` and `tangent[3]` (the handedness) are never touched by skinning.
struct SkinVertex {
    f32 position[3];
    f32 normal[3];
    f32 uv[2];
    f32 tangent[4];
};
static_assert(sizeof(SkinVertex) == sizeof(f32) * 12);

// The inverse-transpose of each skin matrix's linear part, which is what normals are transformed by.
void ComputeSkinNormalMatrices(std::span<const Mat4> skin, std::vector<Mat4>& out);

// Skins `count` vertices from `bind` into `out` (which may not alias `bind`). A vertex whose influences carry no valid
// weight keeps its bind pose. Position, normal and tangent are blended over the four influences and normalised, and the
// tangent is made orthogonal to the skinned normal. Joints outside either matrix span are ignored.
//
// This is the loop the player always used, moved here so it can be tested and run in parallel. A variant that blended the
// joint matrices once per vertex was tried and was not faster (see docs/design/PERFORMANCE.md), so there is one version.
void SkinVertices(const SkinVertex* bind, SkinVertex* out, const std::array<u16, 4>* joints,
                  const std::array<f32, 4>* weights, usize count, std::span<const Mat4> skin,
                  std::span<const Mat4> skin_normal);

// The same, spread over `jobs` in chunks of at least `min_chunk` vertices; null `jobs` or a small mesh runs serially.
void SkinVerticesParallel(JobSystem* jobs, const SkinVertex* bind, SkinVertex* out, const std::array<u16, 4>* joints,
                          const std::array<f32, 4>* weights, usize count, std::span<const Mat4> skin,
                          std::span<const Mat4> skin_normal, usize min_chunk = 1024);

}  // namespace aether::assets
