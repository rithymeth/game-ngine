#include "core/play_session.h"

#include "aether/core/log.h"
#include "aether/scene/serialization.h"

#include <atomic>

namespace aether::editor {

namespace {

std::atomic<u64> next_session_id{1};

std::vector<Entity> AllEntities(const World& world) {
    std::vector<Entity> entities;
    world.ForEachArchetype([&](const Archetype& archetype) {
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* chunk = archetype.EntityArray(c);
            entities.insert(entities.end(), chunk, chunk + archetype.ChunkEntityCount(c));
        }
    });
    return entities;
}

} // namespace

void PlaySession::Play(CommandContext& ctx, CommandStack& stack, bool simulate) {
    if (state_ == State::Paused) {
        pending_steps_ = 0; // a queued Step belongs to the pause we are leaving
        state_ = State::Playing;
        return;
    }
    if (state_ != State::Editing) {
        return;
    }
    snapshot_ = SaveSceneToMemory(ctx.world);
    id_ = next_session_id.fetch_add(1, std::memory_order_relaxed);
    stack.BreakMergeChain();
    stack.SetFrozen(true);
    state_ = State::Playing;
    pending_steps_ = 0;
    simulate_ = simulate;
}

void PlaySession::Pause() {
    if (state_ == State::Playing) {
        state_ = State::Paused;
    }
}

void PlaySession::Step() {
    if (state_ == State::Paused) {
        pending_steps_ = static_cast<u8>(StepTarget::Default) | static_cast<u8>(StepTarget::GameView) |
                         static_cast<u8>(StepTarget::SceneView);
    }
}

bool PlaySession::ShouldSimulate(StepTarget target) {
    if (state_ == State::Playing) {
        return true;
    }
    const u8 bit = static_cast<u8>(target);
    if (state_ == State::Paused && (pending_steps_ & bit) != 0) {
        pending_steps_ &= static_cast<u8>(~bit);
        return true;
    }
    return false;
}

void PlaySession::Stop(CommandContext& ctx, CommandStack& stack) {
    if (state_ == State::Editing) {
        return;
    }
    if (ctx.hooks != nullptr && ctx.hooks->on_entity_destroying) {
        for (Entity entity : AllEntities(ctx.world)) {
            ctx.hooks->on_entity_destroying(entity);
        }
    }
    ctx.world = World();
    if (!LoadSceneFromMemory(ctx.world, snapshot_, "Play-in-Editor snapshot")) {
        AETHER_LOG_ERROR("Editor", "Couldn't restore the edited world after Play; it has been lost");
    }
    ctx.guids.Rebuild(ctx.world);
    if (ctx.hooks != nullptr && ctx.hooks->on_entity_created) {
        for (Entity entity : AllEntities(ctx.world)) {
            ctx.hooks->on_entity_created(entity);
        }
    }
    snapshot_.clear();
    snapshot_.shrink_to_fit();
    stack.SetFrozen(false);
    state_ = State::Editing;
    pending_steps_ = 0;
    simulate_ = false;
}

} // namespace aether::editor
