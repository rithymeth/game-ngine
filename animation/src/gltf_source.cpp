#include "aether/animation/gltf_source.h"

#include <algorithm>

namespace aether::anim {

bool BuildCharacterFromGltf(const assets::GltfScene& scene, u32 skin, GltfCharacter& out, std::string* error) {
    out = {};
    assets::ModelData model;
    std::vector<std::string> names;
    model.nodes.reserve(scene.nodes.size());
    names.reserve(scene.nodes.size());
    for (const assets::GltfNode& n : scene.nodes) {
        assets::ModelNodeData d;
        d.translation = n.translation;
        d.rotation = n.rotation;
        d.scale = n.scale;
        d.matrix = n.matrix;
        d.uses_matrix = n.uses_matrix;
        d.mesh = n.mesh_index;
        d.skin = n.skin_index;
        for (usize c : n.children) d.children.push_back(static_cast<u32>(c));
        model.nodes.push_back(std::move(d));
        names.push_back(n.name);
    }
    for (usize r : scene.root_nodes) model.root_nodes.push_back(static_cast<u32>(r));
    for (const assets::GltfSkin& s : scene.skins) {
        assets::SkinData d;
        for (usize j : s.joints) d.joints.push_back(static_cast<u32>(j));
        d.inverse_bind_matrices = s.inverse_bind_matrices;
        model.skins.push_back(std::move(d));
    }
    if (!BuildSkeleton(model, skin, out.skeleton, &out.bone_nodes, &names, error)) return false;
    for (usize i = 0; i < scene.animations.size(); ++i) {
        const assets::GltfAnimation& a = scene.animations[i];
        assets::AnimationData data;
        data.name = a.name.empty() ? "animation_" + std::to_string(i) : a.name;
        for (const assets::GltfAnimationChannel& c : a.channels) {
            assets::AnimationChannelData ch;
            ch.node = static_cast<u32>(c.node_index);
            ch.path = c.path == assets::GltfAnimationPath::Translation ? assets::AnimationPath::Translation
                      : c.path == assets::GltfAnimationPath::Rotation  ? assets::AnimationPath::Rotation
                                                                       : assets::AnimationPath::Scale;
            ch.interpolation = c.interpolation == assets::GltfAnimationInterpolation::Step ? assets::AnimationInterpolation::Step
                                                                                           : assets::AnimationInterpolation::Linear;
            ch.times = c.times;
            ch.values = c.values;
            if (!ch.times.empty()) data.duration = std::max(data.duration, ch.times.back());
            data.channels.push_back(std::move(ch));
        }
        out.clips.push_back(BuildClip(data, out.skeleton, out.bone_nodes));
        out.clips.back().name = data.name;
    }
    return true;
}

} // namespace aether::anim
