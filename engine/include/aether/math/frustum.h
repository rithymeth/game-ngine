#pragma once

// Phase 38 step 8: the culling test every draw submission needs, whichever side runs it.
//
// `Frustum::FromViewProjection` takes the same matrix the sandbox's compute cull extracts planes
// from, so a CPU cull and a GPU one agree about what is visible by construction rather than by two
// pieces of similar code drifting apart.
//
// The engine's matrices are column-major with v' = M * v and a 0..1 depth range (Vulkan/D3D12), so
// the planes are row combinations of M: left = r3 + r0, right = r3 - r0, bottom = r3 + r1,
// top = r3 - r1, near = r2, far = r3 - r2. A 0..1 depth range is why near is r2 rather than
// r3 + r2, which is the GL convention.

#include "aether/math/mat4.h"
#include "aether/math/vec.h"

#include <cmath>

namespace aether {

struct Plane {
    Vec3 normal;      // unit length
    f32 distance = 0; // dot(normal, point) + distance >= 0 is inside
};

class Frustum {
public:
    // Order: left, right, bottom, top, near, far.
    static Frustum FromViewProjection(const Mat4& matrix) {
        Frustum frustum;
        // Vec4 has named components, not an index operator, so a row is assembled by picking one
        // component out of each column.
        const auto component = [](const Vec4& v, usize index) {
            return index == 0 ? v.x : index == 1 ? v.y : index == 2 ? v.z : v.w;
        };
        const auto row = [&matrix, &component](usize r) {
            return Vec4(component(matrix.cols[0], r), component(matrix.cols[1], r), component(matrix.cols[2], r),
                        component(matrix.cols[3], r));
        };
        const Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        frustum.Set(0, r3 + r0);
        frustum.Set(1, r3 - r0);
        frustum.Set(2, r3 + r1);
        frustum.Set(3, r3 - r1);
        frustum.Set(4, r2);      // 0..1 depth: near is just r2
        frustum.Set(5, r3 - r2);
        return frustum;
    }

    const Plane& operator[](usize i) const { return planes_[i]; }

    // A sphere is visible when it is not fully behind any one plane. Conservative: a sphere that only
    // grazes a corner may be reported visible, which costs a draw rather than a hole in the image.
    bool ContainsSphere(const Vec3& center, f32 radius) const {
        for (const Plane& plane : planes_) {
            if (plane.normal.x * center.x + plane.normal.y * center.y + plane.normal.z * center.z + plane.distance < -radius) return false;
        }
        return true;
    }

    // The same test for a box, using the corner furthest along each plane's normal: if even that one
    // is outside, the whole box is.
    bool ContainsAabb(const Vec3& min, const Vec3& max) const {
        for (const Plane& plane : planes_) {
            const Vec3 positive(plane.normal.x >= 0.0f ? max.x : min.x, plane.normal.y >= 0.0f ? max.y : min.y,
                                plane.normal.z >= 0.0f ? max.z : min.z);
            if (plane.normal.x * positive.x + plane.normal.y * positive.y + plane.normal.z * positive.z + plane.distance < 0.0f) return false;
        }
        return true;
    }

private:
    void Set(usize index, const Vec4& raw) {
        const f32 length = std::sqrt(raw.x * raw.x + raw.y * raw.y + raw.z * raw.z);
        if (!(length > 0.0f)) {
            planes_[index] = Plane{Vec3(0.0f, 0.0f, 0.0f), -1.0f}; // degenerate: cull nothing
            return;
        }
        planes_[index] = Plane{Vec3(raw.x / length, raw.y / length, raw.z / length), raw.w / length};
    }

    Plane planes_[6];
};

} // namespace aether