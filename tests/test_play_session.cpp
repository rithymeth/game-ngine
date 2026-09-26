// Play-in-Editor (editor/src/core/play_session.h), headless.

#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"
#include "core/commands.h"
#include "core/play_session.h"
#include "test_framework.h"

#include <string>

using namespace aether;
using namespace aether::editor;

namespace {

struct Fixture {
    World world;
    GuidIndex guids;
    EditorHooks hooks;
    CommandContext ctx{world, guids, &hooks};
    CommandStack stack;
    PlaySession session;
    int created = 0;
    int destroying = 0;

    Fixture() {
        hooks.on_entity_created = [this](Entity) { ++created; };
        hooks.on_entity_destroying = [this](Entity) { ++destroying; };
    }
};

} // namespace

AETHER_TEST(PlaySession_StopRestoresTheEditedWorldExactly) {
    Fixture f;
    Entity parent = f.world.CreateEntity(Transform{Vec3(1, 2, 3), Quaternion::Identity()});
    Entity child = f.world.CreateEntity(Transform{Vec3(0, 1, 0), Quaternion::Identity()}, ModelRenderer{});
    SetModelPath(*f.world.GetComponent<ModelRenderer>(child), "models/test_cube.gltf");
    EnsureAllGuids(f.world, f.guids);
    f.world.AddComponent(child, Parent{f.world.GetComponent<IdComponent>(parent)->guid});
    // A gap in entity indices, like a real editing session leaves behind.
    Entity doomed = f.world.CreateEntity(Transform{});
    f.world.DestroyEntity(doomed);

    const std::vector<u8> before = SaveSceneToMemory(f.world);
    const EntityGuid child_guid = f.world.GetComponent<IdComponent>(child)->guid;

    f.session.Play(f.ctx, f.stack);
    AETHER_CHECK(f.session.GetState() == PlaySession::State::Playing);
    AETHER_CHECK(f.session.SnapshotBytes() == before.size());

    // "Gameplay": move things, spawn, destroy.
    f.world.GetComponent<Transform>(parent)->position = Vec3(99, 99, 99);
    f.world.CreateEntity(Transform{Vec3(5, 5, 5), Quaternion::Identity()});
    f.world.DestroyEntity(child);
    AETHER_CHECK(f.world.EntityCount() == 2);

    f.session.Stop(f.ctx, f.stack);
    AETHER_CHECK(f.session.IsEditing());
    AETHER_CHECK(SaveSceneToMemory(f.world) == before); // byte-identical
    AETHER_CHECK(f.destroying == 2 && f.created == 2); // hooks saw the swap

    // GUIDs survive, so selection and hierarchy carry over.
    Entity restored_child = f.guids.Find(f.world, child_guid);
    AETHER_CHECK(!restored_child.IsNull());
    AETHER_CHECK(std::string(f.world.GetComponent<ModelRenderer>(restored_child)->asset_path) == "models/test_cube.gltf");
    AETHER_CHECK(!GetParent(f.world, f.guids, restored_child).IsNull());
}

AETHER_TEST(PlaySession_HistoryIsFrozenDuringPlay) {
    Fixture f;
    Entity e = f.world.CreateEntity(Transform{});
    EntityGuid guid = EnsureGuid(f.world, e, &f.guids);
    f.stack.Execute(f.ctx, std::make_unique<AddComponentCommand>(guid, GetComponentId<ModelRenderer>()));
    AETHER_CHECK(f.stack.UndoCount() == 1);

    f.session.Play(f.ctx, f.stack);
    AETHER_CHECK(f.stack.IsFrozen());
    AETHER_CHECK(!f.stack.CanUndo());
    AETHER_CHECK(!f.stack.Undo(f.ctx));
    // A play-time edit still applies, but isn't recorded.
    f.stack.Execute(f.ctx, std::make_unique<RemoveComponentCommand>(guid, GetComponentId<ModelRenderer>()));
    AETHER_CHECK(!f.world.HasComponent<ModelRenderer>(f.guids.Find(f.world, guid)));
    AETHER_CHECK(f.stack.UndoCount() == 1);

    f.session.Stop(f.ctx, f.stack);
    AETHER_CHECK(!f.stack.IsFrozen());
    AETHER_CHECK(f.world.HasComponent<ModelRenderer>(f.guids.Find(f.world, guid))); // play edit discarded
    AETHER_CHECK(f.stack.Undo(f.ctx)); // the pre-play history still works on the restored world
    AETHER_CHECK(!f.world.HasComponent<ModelRenderer>(f.guids.Find(f.world, guid)));
}

AETHER_TEST(PlaySession_PauseAndStep) {
    Fixture f;
    f.world.CreateEntity(Transform{});
    AETHER_CHECK(!f.session.ShouldSimulate()); // editing: nothing runs

    f.session.Play(f.ctx, f.stack);
    AETHER_CHECK(f.session.ShouldSimulate() && f.session.ShouldSimulate());

    f.session.Pause();
    AETHER_CHECK(f.session.GetState() == PlaySession::State::Paused);
    AETHER_CHECK(!f.session.ShouldSimulate());
    f.session.Step();
    AETHER_CHECK(f.session.ShouldSimulate());  // exactly one frame
    AETHER_CHECK(!f.session.ShouldSimulate());

    f.session.Play(f.ctx, f.stack); // resume (no new snapshot)
    AETHER_CHECK(f.session.GetState() == PlaySession::State::Playing);
    f.session.Step(); // ignored while playing
    AETHER_CHECK(f.session.ShouldSimulate());

    f.session.Stop(f.ctx, f.stack);
    f.session.Stop(f.ctx, f.stack); // no-op when already editing
    f.session.Pause();              // no-op when editing
    AETHER_CHECK(f.session.IsEditing() && f.world.EntityCount() == 1);
}
