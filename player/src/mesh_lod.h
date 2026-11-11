#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"

#include <vector>

namespace aether::player {

// A cheaper index list for the same vertices, for a mesh far from the camera. Greedy edge collapse: the shortest edge goes first, one end joining the other, until
// about `target_ratio` of the triangles are left (0.25 keeps a quarter). The end that is farther from the mesh's middle is the one that stays, so a tip stays a tip
// (the other way round a tree gets shorter and its crown thinner); the leaf masses come out a little larger and blunter, which is what distance hides. No vertex is
// made or moved, so the vertex buffer is shared with the full-detail draw and the level costs only an index buffer. Vertices at one place (a flat-shaded mesh has one
// per face) are welded while collapsing, so no crack opens between faces. A mesh that cannot get under `max_ratio` (a quad, a mesh already coarse) returns an empty list,
// meaning "no LOD".
// How much of a mesh its far level keeps: a small mesh has little to spare (a shrub's thin stems go first at a quarter), a big one can lose three quarters.
inline f32 LodTargetRatio(usize triangle_count) { return triangle_count < 1000 ? 0.4f : 0.25f; }

std::vector<u32> SimplifyIndices(const std::vector<Vec3>& positions, const std::vector<u32>& indices, f32 target_ratio = 0.25f, f32 max_ratio = 0.7f);

} // namespace aether::player
