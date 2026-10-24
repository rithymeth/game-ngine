#pragma once

#include "aether/animation/clip.h"
#include "aether/assets/gltf_loader.h"

#include <string>
#include <vector>

namespace aether::anim {

// A skinned character read straight from a loaded glTF scene (what a cooked
// package ships), for hosts that have a GltfScene rather than the importer's
// ModelData: the player's animation host (M8.6). The skeleton and clips are
// built exactly as BuildSkeleton/BuildClip build them from an imported model,
// with bones named after the glTF nodes.
struct GltfCharacter {
    Skeleton skeleton;
    std::vector<u32> bone_nodes;      // the glTF node of each bone
    std::vector<AnimationClip> clips; // one per glTF animation, by its name ("animation_<i>" when unnamed)
};

// False (with `error`) when the scene has no skin `skin` or it isn't a valid skeleton.
bool BuildCharacterFromGltf(const assets::GltfScene& scene, u32 skin, GltfCharacter& out, std::string* error = nullptr);

} // namespace aether::anim
