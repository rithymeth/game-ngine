#include "core/play_session.h"

#include "aether/core/log.h"
#include "aether/scene/serialization.h"

namespace aether::editor {

namespace {

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

void PlaySession::Play(CommandContext& ctx, CommandStack& stack) {
    if (state_ == State::Paused) {
        state_ = State::Playing;
        return;
    }
    if (state_ != State::Editing) {
        return;
    }
    snapshot_ = SaveSceneToMemory(ctx.world);
    stack.BreakMergeChain();
    stack.SetFrozen(true);
    state_ = State::Playing;
    step_pending_ = false;
}

void PlaySession::Pause() {
    if (state_ == State::Playing) {
        state_ = State::Paused;
    }
}

void PlaySession::Step() {
    if (state_ == State::Paused) {
        step_pending_ = true;
    }
}

bool PlaySession::ShouldSimulate() {
    if (state_ == State::Playing) {
        return true;
    }
    if (state_ == State::Paused && step_pending_) {
        step_pending_ = false;
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
    step_pending_ = false;
}

} // namespace aether::editor
