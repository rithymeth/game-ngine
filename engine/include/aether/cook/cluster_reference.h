#pragma once

// @stability: experimental
// CPU reference for selecting cooked meshlet LODs before GPU traversal exists.
// This deliberately uses a single chain per primitive, not yet a spatial cluster DAG.
// The next cooker change must introduce per-cluster parents and conservative error bounds.

#include "aether/cook/mesh_cook.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace aether::cook {

struct ClusterReferenceCamera {
    Vec3 position{};
    float vertical_fov_radians = 1.04719755f;
    float viewport_height_pixels = 1080.0f;
    float maximum_error_pixels = 2.0f;
};

struct ClusterReferenceSelection {
    u32 lod_index = 0;
    std::vector<u32> visible_meshlets;
};

// For a conservative test use the nearest point of the bounding sphere.
// Caller supplies a world-space camera and untransformed mesh (model-space).
inline float ProjectedClusterErrorPixels(float error_world, float distance_to_nearest_bound,
                                         const ClusterReferenceCamera& camera) {
    if (!(error_world >= 0.0f) || !std::isfinite(error_world) ||
        !(camera.vertical_fov_radians > 0.0f) ||
        !(camera.vertical_fov_radians < 3.14159265f) ||
        !(camera.viewport_height_pixels > 0.0f) ||
        !std::isfinite(camera.viewport_height_pixels)) {
        return std::numeric_limits<float>::infinity();
    }
    if (error_world == 0.0f) return 0.0f;
    const float d = std::max(distance_to_nearest_bound, 0.0001f);
    return error_world * camera.viewport_height_pixels /
           (2.0f * d * std::tan(camera.vertical_fov_radians * 0.5f));
}

inline float ClusterReferenceDistance(Vec3 a, Vec3 b) {
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return std::sqrt(x*x + y*y + z*z);
}

// Returns the coarsest LOD whose conservative error fits the pixel budget.
// LodLevel::error is relative to mesh extent; the caller must pass that extent.
// Never assume error increases monotonically: validate each candidate independently.
inline u32 SelectReferenceLod(const CookedMeshPrimitive& primitive, float mesh_extent,
                              const ClusterReferenceCamera& camera) {
    if (primitive.lods.empty()) return 0;
    const Vec3 center{(primitive.bounds_min.x + primitive.bounds_max.x) * 0.5f,
                      (primitive.bounds_min.y + primitive.bounds_max.y) * 0.5f,
                      (primitive.bounds_min.z + primitive.bounds_max.z) * 0.5f};
    const float radius = ClusterReferenceDistance(center, primitive.bounds_max);
    const float distance = std::max(0.0f, ClusterReferenceDistance(camera.position, center) - radius);
    if (!(mesh_extent >= 0.0f) || !std::isfinite(mesh_extent) ||
        !(camera.maximum_error_pixels >= 0.0f) || !std::isfinite(camera.maximum_error_pixels))
        return 0;
    for (usize i = primitive.lods.size(); i-- > 1;) {
        const float error = primitive.lods[i].error;
        if (ProjectedClusterErrorPixels(error * mesh_extent, distance, camera) <=
            camera.maximum_error_pixels) return static_cast<u32>(i);
    }
    return 0;
}

// Conservative sphere-vs-plane test. Plane convention: dot(normal, point) + d >= 0
// is inside. Normals need not be normalized.
struct ClusterReferencePlane {
    Vec3 normal{};
    float d = 0.0f;
};

inline bool ClusterSphereInFrustum(const MeshletBounds& sphere,
                                   const std::vector<ClusterReferencePlane>& planes) {
    for (const auto& plane : planes) {
        const float nx = plane.normal.x, ny = plane.normal.y, nz = plane.normal.z;
        const float nlen = std::sqrt(nx*nx + ny*ny + nz*nz);
        const float signed_distance = nx*sphere.center.x + ny*sphere.center.y +
                                      nz*sphere.center.z + plane.d;
        if (signed_distance < -sphere.radius * nlen) return false;
    }
    return true;
}

// A model-space reference: object transforms must be applied to the camera and
// frustum before calling. Empty planes means no frustum rejection.
inline ClusterReferenceSelection CullReferenceMeshlets(
    const CookedMeshPrimitive& primitive, float mesh_extent,
    const ClusterReferenceCamera& camera, const std::vector<ClusterReferencePlane>& planes) {
    ClusterReferenceSelection result;
    if (primitive.lods.empty()) return result;
    result.lod_index = SelectReferenceLod(primitive, mesh_extent, camera);
    const auto& lod = primitive.lods[result.lod_index];
    const usize count = std::min(lod.meshlets.meshlets.size(), lod.meshlets.bounds.size());
    for (usize i = 0; i < count; ++i) {
        if (ClusterSphereInFrustum(lod.meshlets.bounds[i], planes))
            result.visible_meshlets.push_back(static_cast<u32>(i));
    }
    return result;
}

} // namespace aether::cook
