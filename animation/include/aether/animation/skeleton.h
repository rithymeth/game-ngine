#pragma once

#include "aether/assets/model_importer.h"
#include "aether/math/math.h"

#include <string>
#include <string_view>
#include <vector>

namespace aether::anim {

// Skeletons, poses and blending (Phase 16 step 1, docs/design/PHASE_SPECS.md
// §16.1).

// --- Rotation helpers (the engine's Quaternion is minimal) ----------------------------
f32 Dot(const Quaternion& a, const Quaternion& b);
Quaternion Conjugate(const Quaternion& q); // the inverse of a unit quaternion
Vec3 Rotate(const Quaternion& q, const Vec3& v);
// Normalized lerp along the shorter arc; exact enough for blending nearby poses.
Quaternion Nlerp(const Quaternion& a, const Quaternion& b, f32 t);
Quaternion Slerp(const Quaternion& a, const Quaternion& b, f32 t);
f32 AngleBetween(const Quaternion& a, const Quaternion& b); // radians, 0..pi
Vec3 Lerp(const Vec3& a, const Vec3& b, f32 t);

// A bone's local transform: scale, then rotation, then translation.
struct BoneTransform {
    Vec3 translation{0, 0, 0};
    Quaternion rotation = Quaternion::Identity();
    Vec3 scale{1, 1, 1};
    Mat4 ToMat4() const;
};

// --- Skeleton ------------------------------------------------------------------------------
struct Bone {
    std::string name;
    i32 parent = -1; // always a lower index
    BoneTransform rest;
    Mat4 inverse_bind; // model space -> the bone's bind space
};

struct Skeleton {
    std::vector<Bone> bones;
    usize Size() const { return bones.size(); }
    i32 Find(std::string_view name) const; // -1 if none
    // Parents come before children and names are unique and non-empty.
    bool Valid(std::string* error = nullptr) const;
    bool IsAncestor(i32 ancestor, i32 bone) const; // a bone is its own ancestor
};

// The skeleton of an imported model's skin. Each joint's parent is its
// nearest ancestor node that is also a joint; joints are reordered so
// parents come first. `bone_nodes` receives the model node of each bone
// (what BuildClip maps channels with). Bones are named from `node_names`
// (by node index) when given, else "bone_<node>".
bool BuildSkeleton(const assets::ModelData& model, u32 skin, Skeleton& out, std::vector<u32>* bone_nodes = nullptr,
                   const std::vector<std::string>* node_names = nullptr, std::string* error = nullptr);

// --- Poses ---------------------------------------------------------------------------------
struct Pose {
    std::vector<BoneTransform> local; // one per bone
};
Pose RestPose(const Skeleton& skeleton);
// Model-space transforms, parents first.
void LocalToModel(const Skeleton& skeleton, const Pose& pose, std::vector<Mat4>& model);
// What the skinning shader multiplies vertices by: model * inverse bind.
void SkinMatrices(const Skeleton& skeleton, const std::vector<Mat4>& model, std::vector<Mat4>& out);

// Per-bone weights, 0..1. A mask from a bone covers it and its
// descendants; with `blend_depth` > 0 the weight ramps up over that many
// levels (1/depth at the root bone), for a softer upper-body split.
struct BoneMask {
    std::vector<f32> weights;
};
BoneMask MakeBoneMask(const Skeleton& skeleton, std::string_view root_bone, u32 blend_depth = 0);

// out = a blended toward b by `weight` (times the mask's weight per bone).
// `out` may be `a` or `b`.
void BlendPoses(const Pose& a, const Pose& b, f32 weight, Pose& out, const BoneMask* mask = nullptr);
// Additive animation: the difference from a reference pose, and adding it
// (scaled by `weight`) on top of another pose.
Pose MakeAdditive(const Pose& pose, const Pose& reference);
void ApplyAdditive(Pose& base, const Pose& additive, f32 weight, const BoneMask* mask = nullptr);

} // namespace aether::anim
