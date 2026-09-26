// Tests for the editor's undo/redo stack (editor/src/core/command_stack.h),
// using small test-only commands over a real World. The built-in reflected
// commands (SetField, CreateEntity, ...) come in the next Phase 7 step.

#include "aether/scene/entity_guid.h"
#include "core/command_stack.h"
#include "test_framework.h"

#include <algorithm>
#include <map>
#include <optional>
#include <random>

using namespace aether;
using namespace aether::editor;

namespace {

struct Tag {
    i32 value = 0;
};

Entity Resolve(CommandContext& ctx, const EntityGuid& guid) {
    return ctx.guids.Find(ctx.world, guid);
}

// Sets an entity's Tag; merges consecutive sets of the same entity.
class SetTagCommand final : public ICommand {
public:
    SetTagCommand(EntityGuid guid, i32 old_value, i32 new_value) : guid_(guid), old_(old_value), new_(new_value) {}
    void Do(CommandContext& ctx) override { ctx.world.GetComponent<Tag>(Resolve(ctx, guid_))->value = new_; }
    void Undo(CommandContext& ctx) override { ctx.world.GetComponent<Tag>(Resolve(ctx, guid_))->value = old_; }
    std::string Label() const override { return "Set Tag"; }
    bool TryMerge(const ICommand& next) override {
        auto* other = dynamic_cast<const SetTagCommand*>(&next);
        if (other == nullptr || other->guid_ != guid_) {
            return false;
        }
        new_ = other->new_;
        return true;
    }

private:
    EntityGuid guid_;
    i32 old_;
    i32 new_;
};

// Creates (Do) / destroys (Undo) an entity with a fixed GUID and value.
class CreateCommand final : public ICommand {
public:
    CreateCommand(EntityGuid guid, i32 value) : guid_(guid), value_(value) {}
    void Do(CommandContext& ctx) override {
        Entity e = ctx.world.CreateEntity(IdComponent{guid_}, Tag{value_});
        ctx.guids.Add(guid_, e);
    }
    void Undo(CommandContext& ctx) override {
        ctx.world.DestroyEntity(Resolve(ctx, guid_));
        ctx.guids.Remove(guid_);
    }
    std::string Label() const override { return "Create"; }

private:
    EntityGuid guid_;
    i32 value_;
};

// Destroys (Do) / recreates (Undo) an entity, capturing its value on Do.
class DestroyCommand final : public ICommand {
public:
    explicit DestroyCommand(EntityGuid guid) : guid_(guid) {}
    void Do(CommandContext& ctx) override {
        Entity e = Resolve(ctx, guid_);
        value_ = ctx.world.GetComponent<Tag>(e)->value;
        ctx.world.DestroyEntity(e);
        ctx.guids.Remove(guid_);
    }
    void Undo(CommandContext& ctx) override {
        Entity e = ctx.world.CreateEntity(IdComponent{guid_}, Tag{value_}); // new handle, same identity
        ctx.guids.Add(guid_, e);
    }
    std::string Label() const override { return "Destroy"; }

private:
    EntityGuid guid_;
    i32 value_ = 0;
};

// A command with a declared size, for the memory budget.
class SizedCommand final : public ICommand {
public:
    explicit SizedCommand(usize bytes) : bytes_(bytes) {}
    void Do(CommandContext&) override {}
    void Undo(CommandContext&) override {}
    std::string Label() const override { return "Sized"; }
    usize MemoryBytes() const override { return bytes_; }

private:
    usize bytes_;
};

// Canonical snapshot of the world: GUID -> Tag value. Entity handles and
// storage order differ after undo recreates entities, so compare identity
// and content, not raw bytes.
std::map<std::pair<u64, u64>, i32> Snapshot(World& world) {
    std::map<std::pair<u64, u64>, i32> snapshot;
    world.ForEach<IdComponent, Tag>([&](IdComponent& id, Tag& tag) { snapshot[{id.guid.hi, id.guid.lo}] = tag.value; });
    return snapshot;
}

struct Fixture {
    World world;
    GuidIndex guids;
    CommandContext ctx{world, guids};
    CommandStack stack;

    EntityGuid Create(i32 value) {
        EntityGuid guid = NewEntityGuid();
        stack.Execute(ctx, std::make_unique<CreateCommand>(guid, value));
        return guid;
    }
    i32 Value(const EntityGuid& guid) { return world.GetComponent<Tag>(guids.Find(world, guid))->value; }
    void Set(const EntityGuid& guid, i32 value, MergePolicy merge = MergePolicy::Never) {
        stack.Execute(ctx, std::make_unique<SetTagCommand>(guid, Value(guid), value), merge);
    }
};

} // namespace

AETHER_TEST(CommandStack_UndoRedoRestoresState) {
    Fixture f;
    EntityGuid a = f.Create(1);
    f.Set(a, 5);
    AETHER_CHECK(f.Value(a) == 5);
    AETHER_CHECK(f.stack.UndoLabel() == "Set Tag");

    AETHER_CHECK(f.stack.Undo(f.ctx));
    AETHER_CHECK(f.Value(a) == 1);
    AETHER_CHECK(f.stack.RedoLabel() == "Set Tag");
    AETHER_CHECK(f.stack.Undo(f.ctx));
    AETHER_CHECK(f.guids.Find(f.world, a).IsNull() && f.world.EntityCount() == 0);
    AETHER_CHECK(!f.stack.Undo(f.ctx)); // nothing left

    AETHER_CHECK(f.stack.Redo(f.ctx) && f.stack.Redo(f.ctx));
    AETHER_CHECK(f.Value(a) == 5);
    AETHER_CHECK(!f.stack.Redo(f.ctx));

    // A new command after undo discards the redo history.
    f.stack.Undo(f.ctx);
    f.Set(a, 9);
    AETHER_CHECK(f.stack.RedoCount() == 0 && !f.stack.CanRedo());
    AETHER_CHECK(f.Value(a) == 9);
}

AETHER_TEST(CommandStack_UndoOfDestroyKeepsIdentity) {
    Fixture f;
    EntityGuid a = f.Create(42);
    Entity before = f.guids.Find(f.world, a);
    f.stack.Execute(f.ctx, std::make_unique<DestroyCommand>(a));
    AETHER_CHECK(f.guids.Find(f.world, a).IsNull());
    f.stack.Undo(f.ctx);
    Entity after = f.guids.Find(f.world, a);
    AETHER_CHECK(!after.IsNull() && f.Value(a) == 42);
    // The old handle is dead even if the slot was reused; only the GUID carries over.
    AETHER_CHECK(!f.world.IsAlive(before) || before == after);
    // Commands recorded before the destroy still apply to the recreated entity.
    f.stack.Undo(f.ctx); // undo Create
    f.stack.Redo(f.ctx);
    f.stack.Redo(f.ctx); // redo Destroy
    AETHER_CHECK(f.guids.Find(f.world, a).IsNull());
}

AETHER_TEST(CommandStack_MergesDragsUntilChainBreaks) {
    Fixture f;
    EntityGuid a = f.Create(0);
    EntityGuid b = f.Create(0);
    usize base = f.stack.UndoCount();

    // One "drag": many values, one undo entry.
    for (int v = 1; v <= 10; ++v) {
        f.Set(a, v, MergePolicy::Allow);
    }
    AETHER_CHECK(f.stack.UndoCount() == base + 1);
    AETHER_CHECK(f.Value(a) == 10);

    // Commit (mouse released): the next drag is a separate entry.
    f.stack.BreakMergeChain();
    f.Set(a, 20, MergePolicy::Allow);
    f.Set(a, 30, MergePolicy::Allow);
    AETHER_CHECK(f.stack.UndoCount() == base + 2);

    // A different entity never merges into this entry.
    f.Set(b, 7, MergePolicy::Allow);
    AETHER_CHECK(f.stack.UndoCount() == base + 3);

    f.stack.Undo(f.ctx);
    AETHER_CHECK(f.Value(b) == 0);
    f.stack.Undo(f.ctx);
    AETHER_CHECK(f.Value(a) == 10); // back to the end of the first drag
    f.stack.Undo(f.ctx);
    AETHER_CHECK(f.Value(a) == 0);  // the whole first drag in one step

    // Merging never reaches back past an undo.
    f.stack.Redo(f.ctx);
    f.Set(a, 99, MergePolicy::Allow);
    AETHER_CHECK(f.stack.UndoCount() == base + 2);
}

AETHER_TEST(CommandStack_TransactionsUndoAsOne) {
    Fixture f;
    f.stack.BeginTransaction("Duplicate 3");
    EntityGuid a = f.Create(1);
    f.stack.BeginTransaction("inner (label ignored)");
    EntityGuid b = f.Create(2);
    f.Set(b, 3);
    f.stack.EndTransaction();
    EntityGuid c = f.Create(4);
    AETHER_CHECK(f.stack.InTransaction());
    AETHER_CHECK(!f.stack.CanUndo()); // not while a transaction is open
    AETHER_CHECK(!f.stack.Undo(f.ctx));
    f.stack.EndTransaction();

    AETHER_CHECK(f.stack.UndoCount() == 1);
    AETHER_CHECK(f.stack.UndoLabel() == "Duplicate 3");
    AETHER_CHECK(f.world.EntityCount() == 3 && f.Value(b) == 3);

    f.stack.Undo(f.ctx);
    AETHER_CHECK(f.world.EntityCount() == 0);
    f.stack.Redo(f.ctx);
    AETHER_CHECK(f.Value(a) == 1 && f.Value(b) == 3 && f.Value(c) == 4);

    // An empty transaction adds nothing.
    f.stack.BeginTransaction("nothing");
    f.stack.EndTransaction();
    AETHER_CHECK(f.stack.UndoCount() == 1);
}

AETHER_TEST(CommandStack_TracksUnsavedChanges) {
    Fixture f;
    AETHER_CHECK(!f.stack.IsDirty());
    EntityGuid a = f.Create(1);
    AETHER_CHECK(f.stack.IsDirty());
    f.stack.MarkSaved();
    AETHER_CHECK(!f.stack.IsDirty());

    f.Set(a, 2);
    AETHER_CHECK(f.stack.IsDirty());
    f.stack.Undo(f.ctx);
    AETHER_CHECK(!f.stack.IsDirty()); // back at the saved state
    f.stack.Undo(f.ctx);
    AETHER_CHECK(f.stack.IsDirty());
    f.stack.Redo(f.ctx);
    AETHER_CHECK(!f.stack.IsDirty());

    // Undo past the save, then do something new: the saved state is gone for good.
    f.stack.Undo(f.ctx);
    f.Create(5);
    AETHER_CHECK(f.stack.IsDirty());
    f.stack.Undo(f.ctx);
    AETHER_CHECK(f.stack.IsDirty());

    // A drag right after saving doesn't merge into (and silently change) the saved entry.
    Fixture g;
    EntityGuid x = g.Create(0);
    g.Set(x, 1, MergePolicy::Allow);
    g.stack.MarkSaved();
    g.Set(x, 2, MergePolicy::Allow);
    AETHER_CHECK(g.stack.IsDirty());
    g.stack.Undo(g.ctx);
    AETHER_CHECK(!g.stack.IsDirty() && g.Value(x) == 1);
}

AETHER_TEST(CommandStack_MemoryBudgetDropsOldestEntries) {
    Fixture f;
    f.stack.SetMemoryBudget(1000);
    for (int i = 0; i < 10; ++i) {
        f.stack.Execute(f.ctx, std::make_unique<SizedCommand>(300));
    }
    AETHER_CHECK(f.stack.UndoCount() == 3); // 900 <= 1000 < 1200
    AETHER_CHECK(f.stack.MemoryUsed() == 900);

    f.stack.Execute(f.ctx, std::make_unique<SizedCommand>(5000)); // alone over budget: still kept
    AETHER_CHECK(f.stack.UndoCount() == 1 && f.stack.MemoryUsed() == 5000);

    // Dropping the entry that leads back to the saved state makes it
    // unreachable: saved at depth 0, then the first entry is dropped.
    Fixture g;
    g.stack.SetMemoryBudget(1000);
    g.stack.MarkSaved();
    g.stack.Execute(g.ctx, std::make_unique<SizedCommand>(600));
    g.stack.Execute(g.ctx, std::make_unique<SizedCommand>(600)); // drops the first
    g.stack.Undo(g.ctx);
    AETHER_CHECK(!g.stack.CanUndo());
    AETHER_CHECK(g.stack.IsDirty());

    // But a saved state *after* the dropped entry is still reachable.
    Fixture h;
    h.stack.SetMemoryBudget(1000);
    h.stack.Execute(h.ctx, std::make_unique<SizedCommand>(600));
    h.stack.MarkSaved();
    h.stack.Execute(h.ctx, std::make_unique<SizedCommand>(600)); // drops the first
    AETHER_CHECK(h.stack.IsDirty());
    h.stack.Undo(h.ctx);
    AETHER_CHECK(!h.stack.IsDirty()); // back at the state right after the first command
}

AETHER_TEST(CommandStack_ThousandRandomCommandsUndoAndRedoExactly) {
    // docs/design/PHASE_SPECS.md §7.2: random commands, undo all -> the
    // original state; redo all -> the state after the random run.
    Fixture f;
    for (int i = 0; i < 20; ++i) {
        f.Create(i); // starting content
    }
    f.stack.MarkSaved();
    const auto original = Snapshot(f.world);
    const usize base_depth = f.stack.UndoCount();

    std::mt19937 rng(12345);
    std::vector<EntityGuid> alive;
    f.world.ForEach<IdComponent>([&](IdComponent& id) { alive.push_back(id.guid); });
    std::sort(alive.begin(), alive.end(), [](auto& a, auto& b) { return a.hi < b.hi; });

    for (int i = 0; i < 1000; ++i) {
        int kind = std::uniform_int_distribution<int>(0, 9)(rng);
        if (kind < 2 || alive.empty()) {
            alive.push_back(f.Create(static_cast<i32>(rng() % 1000)));
        } else if (kind < 4) {
            usize pick = rng() % alive.size();
            f.stack.Execute(f.ctx, std::make_unique<DestroyCommand>(alive[pick]));
            alive.erase(alive.begin() + static_cast<std::ptrdiff_t>(pick));
        } else if (kind < 5) {
            f.stack.BeginTransaction("batch");
            for (int k = 0; k < 3 && !alive.empty(); ++k) {
                f.Set(alive[rng() % alive.size()], static_cast<i32>(rng() % 1000));
            }
            f.stack.EndTransaction();
        } else {
            MergePolicy merge = (rng() % 2) ? MergePolicy::Allow : MergePolicy::Never;
            if (rng() % 5 == 0) {
                f.stack.BreakMergeChain();
            }
            f.Set(alive[rng() % alive.size()], static_cast<i32>(rng() % 1000), merge);
        }
    }
    const auto after_run = Snapshot(f.world);
    AETHER_CHECK(after_run != original);

    while (f.stack.UndoCount() > base_depth) {
        AETHER_CHECK(f.stack.Undo(f.ctx));
    }
    AETHER_CHECK(Snapshot(f.world) == original);
    AETHER_CHECK(!f.stack.IsDirty());
    AETHER_CHECK(f.guids.Rebuild(f.world).empty());

    while (f.stack.CanRedo()) {
        AETHER_CHECK(f.stack.Redo(f.ctx));
    }
    AETHER_CHECK(Snapshot(f.world) == after_run);
    AETHER_CHECK(f.stack.IsDirty());
}
