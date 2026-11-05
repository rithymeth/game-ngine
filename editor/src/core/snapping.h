#pragma once

// Phase 43.3: gizmo snapping.
//
// A gizmo turns a drag into a move, a rotation or a scale; snapping rounds the result to a step
// so a scene can be laid out by hand. These are the rules, kept out of the drawing code so they
// can be checked without a viewport.

#include "aether/math/vec.h"

#include <cmath>

namespace aether::editor {

// Rounds to the nearest multiple of `step`. A step of zero or less means "no snapping", so
// turning snapping off is the same call with a zero step rather than a separate path.
inline f32 SnapScalar(f32 value, f32 step) {
    if (!(step > 0.0f)) return value;
    return std::round(value / step) * step;
}

inline Vec3 SnapTranslation(const Vec3& position, f32 step) {
    return Vec3(SnapScalar(position.x, step), SnapScalar(position.y, step), SnapScalar(position.z, step));
}

inline f32 SnapRotationDegrees(f32 degrees, f32 step) { return SnapScalar(degrees, step); }

// Scale snaps but never collapses to zero: rounding a small axis to the grid would make the
// object vanish, which is never what a drag to "almost nothing" means. It lands on +/- one step
// instead, keeping the sign the drag gave it.
inline f32 SnapScaleAxis(f32 value, f32 step) {
    if (!(step > 0.0f)) return value;
    const f32 snapped = SnapScalar(value, step);
    if (snapped != 0.0f) return snapped;
    return value < 0.0f ? -step : step;
}

inline Vec3 SnapScale(const Vec3& scale, f32 step) {
    return Vec3(SnapScaleAxis(scale.x, step), SnapScaleAxis(scale.y, step), SnapScaleAxis(scale.z, step));
}

} // namespace aether::editor
