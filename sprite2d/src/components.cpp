#include "aether/sprite2d/components.h"

#include <cmath>

namespace aether::sprite2d {

void UpdateSpriteAnimations(World& world, const AtlasResolver& atlases, f32 dt) {
    UpdateSpriteAnimations(world, atlases, dt, {});
}

void UpdateSpriteAnimations(World& world, const AtlasResolver& atlases, f32 dt, const SpriteClipReporter& report) {
    if (!std::isfinite(dt)) return;
    world.ForEach<Sprite, SpriteAnimator>([&](Sprite& sprite_ref, SpriteAnimator& animator_ref) {
        Sprite* sprite = &sprite_ref;
        SpriteAnimator* animator = &animator_ref;
        if (!animator->playing) return;
        const SpriteAtlas* atlas = atlases ? atlases(sprite->atlas.guid) : nullptr;
        const SpriteClip* clip = atlas ? atlas->FindClip(animator->clip) : nullptr;
        if (!clip || clip->frames.empty()) {
            if (report && atlas && animator->clip[0]) report(sprite->atlas.guid, animator->clip, clip != nullptr);
            return;
        }
        const f32 next_time = animator->time + dt * animator->speed;
        // Skip a bad advance rather than poisoning the playhead for all later frames.
        if (!std::isfinite(next_time)) return;
        animator->time = next_time;
        bool finished = false;
        const usize index = ClipFrameAt(*clip, animator->time, &finished);
        SetSpriteFrame(*sprite, clip->frames[index]);
        if (finished) {
            animator->finished = true;
            animator->playing = false;
        }
    });
}

void RegisterSprite2DComponents() {
    (void)GetComponentId<Sprite>();
    (void)GetComponentId<SpriteAnimator>();
    (void)GetComponentId<TilemapRenderer>();
}

} // namespace aether::sprite2d
