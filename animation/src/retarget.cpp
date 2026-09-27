#include "aether/animation/retarget.h"

#include "aether/animation/ik.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace aether::anim {

namespace {

// "mixamorig:Left_Arm" -> "leftarm".
std::string Canonical(const std::string& name) {
    const usize colon = name.rfind(':');
    std::string out;
    for (usize i = colon == std::string::npos ? 0 : colon + 1; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c == '_' || c == '-' || c == ' ' || c == '.') continue;
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

} // namespace

BoneMapping AutoMapBones(const Skeleton& source, const Skeleton& target) {
    BoneMapping m;
    std::map<std::string, std::string> by_name;
    for (const Bone& b : source.bones) by_name.emplace(Canonical(b.name), b.name);
    for (const Bone& b : target.bones) {
        auto it = by_name.find(Canonical(b.name));
        if (it != by_name.end()) m.bones.push_back({it->second, b.name});
    }
    return m;
}

Retargeter::Retargeter(const Skeleton& source, const Skeleton& target, const BoneMapping& mapping) : source_(source), target_(target) {
    source_of_.assign(target.bones.size(), -1);
    for (const auto& [from, to] : mapping.bones) {
        const i32 s = source.Find(from), t = target.Find(to);
        if (s >= 0 && t >= 0) source_of_[static_cast<usize>(t)] = s;
    }
    root_ = mapping.root.empty() ? -1 : target.Find(mapping.root);
    if (root_ < 0) {
        for (usize i = 0; i < target.bones.size(); ++i) {
            if (source_of_[i] >= 0) {
                root_ = static_cast<i32>(i); // the first mapped bone (parents come first)
                break;
            }
        }
    }
    source_root_ = root_ >= 0 ? source_of_[static_cast<usize>(root_)] : -1;
    std::vector<ModelTransform> sm, tm;
    ComputeModelTransforms(source, RestPose(source), sm);
    ComputeModelTransforms(target, RestPose(target), tm);
    for (const ModelTransform& t : sm) source_rest_model_.push_back(t.rotation);
    for (const ModelTransform& t : tm) target_rest_model_.push_back(t.rotation);
    if (root_ >= 0 && source_root_ >= 0) {
        const f32 sh = sm[static_cast<usize>(source_root_)].position.y, th = tm[static_cast<usize>(root_)].position.y;
        scale_ = std::fabs(sh) > 1e-4f ? th / sh : 1.0f;
    }
}

usize Retargeter::MappedCount() const {
    return static_cast<usize>(std::count_if(source_of_.begin(), source_of_.end(), [](i32 s) { return s >= 0; }));
}

void Retargeter::Retarget(const Pose& source_pose, Pose& out) const {
    std::vector<ModelTransform> sm;
    ComputeModelTransforms(source_, source_pose, sm);
    std::vector<ModelTransform> source_rest;
    ComputeModelTransforms(source_, RestPose(source_), source_rest);
    out = RestPose(target_);
    std::vector<ModelTransform> tm(target_.bones.size());
    for (usize i = 0; i < target_.bones.size(); ++i) {
        const i32 parent = target_.bones[i].parent;
        const ModelTransform parent_model = parent >= 0 ? tm[static_cast<usize>(parent)] : ModelTransform{};
        BoneTransform& local = out.local[i];
        const i32 s = source_of_[i];
        if (s >= 0) {
            // The source bone's turn away from its rest pose, applied to the target's rest.
            const Quaternion delta = sm[static_cast<usize>(s)].rotation * Conjugate(source_rest_model_[static_cast<usize>(s)]);
            const Quaternion model_rotation = (delta * target_rest_model_[i]).Normalized();
            local.rotation = (Conjugate(parent_model.rotation) * model_rotation).Normalized();
        }
        if (static_cast<i32>(i) == root_ && source_root_ >= 0) {
            // The root moves as the source's did, scaled to the target's size.
            const Vec3 moved = (sm[static_cast<usize>(source_root_)].position - source_rest[static_cast<usize>(source_root_)].position) * scale_;
            const Vec3 rest_model = parent_model.position + Rotate(parent_model.rotation, target_.bones[i].rest.translation);
            local.translation = Rotate(Conjugate(parent_model.rotation), rest_model + moved - parent_model.position);
        }
        const Vec3 scaled(local.translation.x * parent_model.scale.x, local.translation.y * parent_model.scale.y,
                          local.translation.z * parent_model.scale.z);
        tm[i] = {parent_model.position + Rotate(parent_model.rotation, scaled), (parent_model.rotation * local.rotation).Normalized(),
                 Vec3(parent_model.scale.x * local.scale.x, parent_model.scale.y * local.scale.y, parent_model.scale.z * local.scale.z)};
    }
}

AnimationClip Retargeter::RetargetClip(const AnimationClip& clip, f32 rate) const {
    AnimationClip out;
    out.name = clip.name;
    out.duration = clip.duration;
    out.notifies = clip.notifies;
    out.tracks.resize(target_.bones.size());
    const u32 frames = std::max(1u, static_cast<u32>(std::ceil(clip.duration * std::max(rate, 1.0f))));
    Pose source_pose, target_pose;
    for (u32 f = 0; f <= frames; ++f) {
        const f32 t = clip.duration * static_cast<f32>(f) / static_cast<f32>(frames);
        SampleClip(clip, source_, t, false, source_pose);
        Retarget(source_pose, target_pose);
        for (usize i = 0; i < target_.bones.size(); ++i) {
            if (source_of_[i] < 0) continue;
            out.tracks[i].rotation.times.push_back(t);
            out.tracks[i].rotation.values.push_back(target_pose.local[i].rotation);
            if (static_cast<i32>(i) == root_) {
                out.tracks[i].translation.times.push_back(t);
                out.tracks[i].translation.values.push_back(target_pose.local[i].translation);
            }
        }
    }
    return out;
}

} // namespace aether::anim
