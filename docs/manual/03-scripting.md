# Scripting

Two ways to make things happen, and they work together.

## Luau scripts

A script is a `.luau` file attached to an entity with a `ScriptComponent`. Its
top-level fields are the settings the Inspector shows, and it defines lifecycle
callbacks:

    speed = 6.0

    function OnStart() end
    function OnUpdate(dt)
        local t = self.entity:Get("Transform")
        local move = Input.GetAxis2D("Move")
        t.position = t.position + vector.create(move.x, 0, -move.y) * (speed * dt)
    end

The world is `world` (`world:EntitiesWith("Camera")`, `world:Spawn`, `Find`,
`Destroy`); an entity has `:Get`, `:Add`, `:Has`, `:Remove`; every component's
reflected fields are readable and writable and its reflected functions are
methods. `Timer.After`, `Timer.Every` and `Event.new()` are there for timing and
signals. A script runs in a sandbox: no files, no network, an instruction
budget per call, a memory cap. A script error is reported, and the game
keeps running.

## Blueprints

A Blueprint (`.abp`) is a node graph with variables, events and functions,
compiled to a small VM. Attach one with a `BlueprintInstance`. The Blueprint
editor (Tools > Scripting) has a compiler with messages, a debugger with
breakpoints, and the node library in
[../design/BLUEPRINT_NODES.md](../design/BLUEPRINT_NODES.md). Anything marked
`Fn_BlueprintCallable` in C++ becomes a node.

## Which to use

Use Luau for logic you would write as code, Blueprints for what a designer
should wire up by sight. Both can call the engine's reflected functions and both
run in the packaged player.
