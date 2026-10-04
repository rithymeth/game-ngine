#include "aether/sprite2d/components.h"

namespace aether::sprite2d {

void UpdateSpriteAnimations(World& world, const AtlasResolver& atlases, f32 dt) {
    world.ForEach<Sprite, SpriteAnimator>([&](Sprite& sprite_ref, SpriteAnimator& animator_ref) {
        Sprite* sprite = &sprite_ref;
        SpriteAnimator* animator = &animator_ref;
        if (!animator->playing) return;
        const SpriteAtlas* atlas = atlases ? atlases(sprite->atlas.guid) : nullptr;
        const SpriteClip* clip = atlas ? atlas->FindClip(animator->clip) : nullptr;
        if (!clip || clip->frames.empty()) return;
        animator->time += dt * animator->speed;
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
