#include "aether/animation/skeleton.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace aether::anim {

// --- Rotation helpers ----------------------------------------------------------------------

f32 Dot(const Quaternion& a, const Quaternion& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

Quaternion Conjugate(const Quaternion& q) { return Quaternion(-q.x, -q.y, -q.z, q.w); }

Vec3 Rotate(const Quaternion& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    const Vec3 t = u.Cross(v) * 2.0f;
    return v + t * q.w + u.Cross(t);
}

Quaternion Nlerp(const Quaternion& a, const Quaternion& b, f32 t) {
    const f32 sign = Dot(a, b) < 0.0f ? -1.0f : 1.0f;
    return Quaternion(a.x + (b.x * sign - a.x) * t, a.y + (b.y * sign - a.y) * t, a.z + (b.z * sign - a.z) * t,
                      a.w + (b.w * sign - a.w) * t)
        .Normalized();
}

Quaternion Slerp(const Quaternion& a, const Quaternion& b, f32 t) {
    f32 d = Dot(a, b);
    Quaternion c = b;
    if (d < 0.0f) {
        d = -d;
        c = Quaternion(-b.x, -b.y, -b.z, -b.w);
    }
    if (d > 0.9995f) return Nlerp(a, c, t);
    const f32 theta = std::acos(std::min(d, 1.0f));
    const f32 s = std::sin(theta);
    const f32 wa = std::sin((1.0f - t) * theta) / s, wb = std::sin(t * theta) / s;
    return Quaternion(a.x * wa + c.x * wb, a.y * wa + c.y * wb, a.z * wa + c.z * wb, a.w * wa + c.w * wb);
}

f32 AngleBetween(const Quaternion& a, const Quaternion& b) {
    // atan2 of the relative rotation's parts: precise for small angles,
    // where acos(dot) loses everything below ~5e-4 rad in floats.
    const Quaternion d = Conjugate(a.Normalized()) * b.Normalized();
    const f32 v = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    return 2.0f * std::atan2(v, std::fabs(d.w));
}

Vec3 Lerp(const Vec3& a, const Vec3& b, f32 t) { return a + (b - a) * t; }

Mat4 BoneTransform::ToMat4() const { return Mat4::Translation(translation) * rotation.ToMat4() * Mat4::Scale(scale); }

namespace {

Vec3 Xyz(const Vec4& v) { return Vec3(v.x, v.y, v.z); }

// A TRS matrix back into its parts (no shear).
BoneTransform Decompose(const Mat4& m) {
    BoneTransform t;
    t.translation = Xyz(m.cols[3]);
    t.scale = Vec3(Xyz(m.cols[0]).Length(), Xyz(m.cols[1]).Length(), Xyz(m.cols[2]).Length());
    if (Xyz(m.cols[0]).Cross(Xyz(m.cols[1])).Dot(Xyz(m.cols[2])) < 0.0f) t.scale.x = -t.scale.x; // a mirror
    const Vec3 c0 = Xyz(m.cols[0]) * (t.scale.x != 0 ? 1.0f / t.scale.x : 0.0f);
    const Vec3 c1 = Xyz(m.cols[1]) * (t.scale.y != 0 ? 1.0f / t.scale.y : 0.0f);
    const Vec3 c2 = Xyz(m.cols[2]) * (t.scale.z != 0 ? 1.0f / t.scale.z : 0.0f);
    // Rotation matrix (columns c0, c1, c2) to a quaternion.
    const f32 trace = c0.x + c1.y + c2.z;
    Quaternion q;
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        q = Quaternion((c1.z - c2.y) / s, (c2.x - c0.z) / s, (c0.y - c1.x) / s, 0.25f * s);
    } else if (c0.x > c1.y && c0.x > c2.z) {
        const f32 s = std::sqrt(1.0f + c0.x - c1.y - c2.z) * 2.0f;
        q = Quaternion(0.25f * s, (c1.x + c0.y) / s, (c2.x + c0.z) / s, (c1.z - c2.y) / s);
    } else if (c1.y > c2.z) {
        const f32 s = std::sqrt(1.0f + c1.y - c0.x - c2.z) * 2.0f;
        q = Quaternion((c1.x + c0.y) / s, 0.25f * s, (c2.y + c1.z) / s, (c2.x - c0.z) / s);
    } else {
        const f32 s = std::sqrt(1.0f + c2.z - c0.x - c1.y) * 2.0f;
        q = Quaternion((c2.x + c0.z) / s, (c2.y + c1.z) / s, 0.25f * s, (c0.y - c1.x) / s);
    }
    t.rotation = q.Normalized();
    return t;
}

Mat4 NodeMatrix(const assets::ModelNodeData& n) {
    if (n.uses_matrix) return n.matrix;
    BoneTransform t;
    t.translation = n.translation;
    t.rotation = n.rotation;
    t.scale = n.scale;
    return t.ToMat4();
}

} // namespace

// --- Skeleton -------------------------------------------------------------------------------

i32 Skeleton::Find(std::string_view name) const {
    for (usize i = 0; i < bones.size(); ++i) {
        if (bones[i].name == name) return static_cast<i32>(i);
    }
    return -1;
}

bool Skeleton::Valid(std::string* error) const {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    std::set<std::string> names;
    for (usize i = 0; i < bones.size(); ++i) {
        const Bone& b = bones[i];
        if (b.name.empty()) return fail("bone " + std::to_string(i) + " has no name");
        if (!names.insert(b.name).second) return fail("two bones are named '" + b.name + "'");
        if (b.parent >= static_cast<i32>(i) || b.parent < -1) return fail("bone '" + b.name + "' comes before its parent");
    }
    return true;
}

bool Skeleton::IsAncestor(i32 ancestor, i32 bone) const {
    for (i32 b = bone; b >= 0; b = bones[static_cast<usize>(b)].parent) {
        if (b == ancestor) return true;
    }
    return false;
}

bool BuildSkeleton(const assets::ModelData& model, u32 skin_index, Skeleton& out, std::vector<u32>* bone_nodes,
                   const std::vector<std::string>* node_names, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (skin_index >= model.skins.size()) return fail("the model has no skin " + std::to_string(skin_index));
    const assets::SkinData& skin = model.skins[skin_index];
    if (skin.joints.empty()) return fail("the skin has no joints");
    std::vector<i32> node_parent(model.nodes.size(), -1);
    for (usize n = 0; n < model.nodes.size(); ++n) {
        for (u32 c : model.nodes[n].children) {
            if (c < node_parent.size()) node_parent[c] = static_cast<i32>(n);
        }
    }
    std::map<u32, usize> joint_index; // node -> index in skin.joints
    for (usize k = 0; k < skin.joints.size(); ++k) {
        if (skin.joints[k] >= model.nodes.size()) return fail("a joint refers to a node that doesn't exist");
        if (!joint_index.emplace(skin.joints[k], k).second) return fail("a node is listed twice as a joint");
    }
    // Each joint's joint parent, and the nodes in between (folded into its rest pose).
    struct Joint {
        u32 node;
        i32 parent_node = -1; // a joint node, or -1
        Mat4 local;           // relative to the parent joint (or the skeleton root)
        u32 depth = 0;
    };
    std::vector<Joint> joints;
    for (u32 node : skin.joints) {
        Joint j{node, -1, NodeMatrix(model.nodes[node]), 0};
        for (i32 p = node_parent[node]; p >= 0; p = node_parent[static_cast<usize>(p)]) {
            if (joint_index.count(static_cast<u32>(p))) {
                j.parent_node = p;
                break;
            }
            j.local = NodeMatrix(model.nodes[static_cast<usize>(p)]) * j.local;
        }
        joints.push_back(j);
    }
    // Non-joint nodes above the root joint are folded into its rest pose
    // too, so model space is the model's own space, which is what the
    // skin's inverse bind matrices expect.
    for (Joint& j : joints) {
        for (i32 p = j.parent_node; p >= 0;) {
            ++j.depth;
            const Joint& up = joints[joint_index.at(static_cast<u32>(p))];
            p = up.parent_node;
            if (j.depth > joints.size()) return fail("the joints form a loop");
        }
    }
    std::vector<usize> order(joints.size());
    for (usize i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](usize a, usize b) { return joints[a].depth < joints[b].depth; });
    std::map<u32, i32> bone_of_node;
    Skeleton skeleton;
    std::vector<u32> nodes;
    for (usize k : order) {
        const Joint& j = joints[k];
        Bone bone;
        bone.name = node_names != nullptr && j.node < node_names->size() && !(*node_names)[j.node].empty() ? (*node_names)[j.node]
                                                                                                          : "bone_" + std::to_string(j.node);
        bone.parent = j.parent_node >= 0 ? bone_of_node.at(static_cast<u32>(j.parent_node)) : -1;
        bone.rest = Decompose(j.local);
        bone.inverse_bind = k < skin.inverse_bind_matrices.size() ? skin.inverse_bind_matrices[k] : Mat4::Identity();
        bone_of_node[j.node] = static_cast<i32>(skeleton.bones.size());
        skeleton.bones.push_back(std::move(bone));
        nodes.push_back(j.node);
    }
    std::string why;
    if (!skeleton.Valid(&why)) return fail(why);
    out = std::move(skeleton);
    if (bone_nodes != nullptr) *bone_nodes = std::move(nodes);
    return true;
}

// --- Poses -----------------------------------------------------------------------------------

Pose RestPose(const Skeleton& skeleton) {
    Pose pose;
    pose.local.reserve(skeleton.bones.size());
    for (const Bone& b : skeleton.bones) pose.local.push_back(b.rest);
    return pose;
}

void LocalToModel(const Skeleton& skeleton, const Pose& pose, std::vector<Mat4>& model) {
    model.resize(skeleton.bones.size());
    for (usize i = 0; i < skeleton.bones.size(); ++i) {
        const Mat4 local = i < pose.local.size() ? pose.local[i].ToMat4() : skeleton.bones[i].rest.ToMat4();
        const i32 parent = skeleton.bones[i].parent;
        model[i] = parent >= 0 ? model[static_cast<usize>(parent)] * local : local;
    }
}

void SkinMatrices(const Skeleton& skeleton, const std::vector<Mat4>& model, std::vector<Mat4>& out) {
    out.resize(skeleton.bones.size());
    for (usize i = 0; i < skeleton.bones.size() && i < model.size(); ++i) out[i] = model[i] * skeleton.bones[i].inverse_bind;
}

BoneMask MakeBoneMask(const Skeleton& skeleton, std::string_view root_bone, u32 blend_depth) {
    BoneMask mask;
    mask.weights.assign(skeleton.bones.size(), 0.0f);
    const i32 root = skeleton.Find(root_bone);
    if (root < 0) return mask;
    for (usize i = 0; i < skeleton.bones.size(); ++i) {
        u32 depth = 0;
        bool inside = false;
        for (i32 b = static_cast<i32>(i); b >= 0; b = skeleton.bones[static_cast<usize>(b)].parent, ++depth) {
            if (b == root) {
                inside = true;
                break;
            }
        }
        if (!inside) continue;
        mask.weights[i] = blend_depth == 0 ? 1.0f : std::min(1.0f, static_cast<f32>(depth + 1) / static_cast<f32>(blend_depth));
    }
    return mask;
}

void BlendPoses(const Pose& a, const Pose& b, f32 weight, Pose& out, const BoneMask* mask) {
    const usize n = std::min(a.local.size(), b.local.size());
    out.local.resize(n);
    for (usize i = 0; i < n; ++i) {
        const f32 w = weight * (mask != nullptr && i < mask->weights.size() ? mask->weights[i] : 1.0f);
        const BoneTransform& x = a.local[i];
        const BoneTransform& y = b.local[i];
        out.local[i] = {Lerp(x.translation, y.translation, w), Nlerp(x.rotation, y.rotation, w), Lerp(x.scale, y.scale, w)};
    }
}

Pose MakeAdditive(const Pose& pose, const Pose& reference) {
    Pose delta;
    const usize n = std::min(pose.local.size(), reference.local.size());
    delta.local.resize(n);
    for (usize i = 0; i < n; ++i) {
        const BoneTransform& p = pose.local[i];
        const BoneTransform& r = reference.local[i];
        auto ratio = [](f32 x, f32 y) { return y != 0.0f ? x / y : 1.0f; };
        delta.local[i] = {p.translation - r.translation, (Conjugate(r.rotation) * p.rotation).Normalized(),
                          Vec3(ratio(p.scale.x, r.scale.x), ratio(p.scale.y, r.scale.y), ratio(p.scale.z, r.scale.z))};
    }
    return delta;
}

void ApplyAdditive(Pose& base, const Pose& additive, f32 weight, const BoneMask* mask) {
    const usize n = std::min(base.local.size(), additive.local.size());
    for (usize i = 0; i < n; ++i) {
        const f32 w = weight * (mask != nullptr && i < mask->weights.size() ? mask->weights[i] : 1.0f);
        BoneTransform& b = base.local[i];
        const BoneTransform& d = additive.local[i];
        b.translation = b.translation + d.translation * w;
        b.rotation = (b.rotation * Nlerp(Quaternion::Identity(), d.rotation, w)).Normalized();
        const Vec3 s = Lerp(Vec3(1, 1, 1), d.scale, w);
        b.scale = Vec3(b.scale.x * s.x, b.scale.y * s.y, b.scale.z * s.z);
    }
}

} // namespace aether::anim
