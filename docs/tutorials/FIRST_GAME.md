# Your first game in Aether: "Coin Run"

> **Status: target workflow.** This tutorial describes how making a game
> will work once milestone **M2 (Scriptable)** from the
> [roadmap](../ROADMAP.md) is done. It doesn't work in today's editor yet.
> It exists to pin down the experience we're building toward, and it's
> the acceptance test for M2: when every step here works as written, M2 is
> done.

**What you'll make:** a small 3D platformer. The player runs and jumps
around a level, collects 10 coins, opens a door, and reaches a goal. No
C++, only Blueprints (with an optional Luau version of one step).

**Time:** about 45 minutes.

**Features this exercises:** projects and templates (Phase 7), the
Content Browser and importing (8), prefabs (9), input actions (10),
Luau (11), Blueprints including latent nodes, dispatchers and interfaces
(12), triggers and character movement (13).

---

## Step 1: create the project

1. Start the editor. The **Project Browser** opens.
2. Choose **New Project from Template › Third Person**.
3. Name it `CoinRun`, pick a folder, leave **Starter content** on, and
   choose **Blueprint** for scripting.
4. Click **Create Project**.

The editor opens with a sample level: a floor, some blocks, a sun and sky,
and `BP_ThirdPersonCharacter` already set as the player.

5. Press **Alt+P** (Play). You can already run with WASD, look with the
   mouse and jump with Space. Press **Esc** to stop. Everything you moved
   during play goes back to how it was.

**Checks Phase 7:** templates, PIE that doesn't change the edited level.

---

## Step 2: build a small level

1. In the Content Browser, open `Content/StarterContent/Props`.
2. Drag `SM_Block` into the viewport several times to make platforms. Use
   **W / E / R** to move, rotate and scale. Turn on grid snapping (0.5 m)
   in the toolbar so the blocks line up.
3. Select a few blocks and press **Ctrl+G** to group them under an empty
   entity named `Platforms`.
4. Made a mistake? **Ctrl+Z** undoes anything, including whole groups.
5. **Ctrl+S** saves the level as `Content/Maps/Level_01`.

**Checks Phases 7 and 8:** gizmos, snapping, grouping, undo, drag from the
Content Browser, saving.

---

## Step 3: make a coin prefab

1. **+ Add › Blueprint Class**, choose parent **Entity**, and name it
   `BP_Coin`. Double-click it to open the Blueprint Editor.
2. In **Components**, add:
   - **Mesh Renderer** with `SM_Coin` (a starter-content mesh) and the
     material `M_Gold`.
   - **Sphere Collider**, radius 0.5, and tick **Is Trigger**.
3. In **My Blueprint › Variables**, add `SpinSpeed` (float, default 180),
   and tick **Instance Editable** so each coin can have its own value.

### Make the coin spin

4. In the **Event Graph**, find **Event Tick**. Drag from its exec pin and
   type `add local rotation`. Choose **Add Local Rotation** (Self).
5. For the Rotator input, right-click › **Split Struct Pin**. Connect a
   **Multiply** node to **Yaw**: `SpinSpeed × Delta Seconds`.

```
◆ Event Tick ──▶ Add Local Rotation (Self)
   Delta Seconds ─┐        Yaw ◀── [ SpinSpeed × Delta Seconds ]
```

### Collect the coin

6. Right-click in the graph › **Event OnTriggerEnter**.
7. Drag from **Other** › **Cast To BP_ThirdPersonCharacter**.
8. From **success**: **Play Sound at Location** (sound `S_CoinPickup`,
   location from **Get World Location (Self)**), then **Get Game Mode** →
   **Cast To BP_CoinGameMode** → **Add Coin** (we'll create both in
   Step 5), then **Destroy Entity (Self)**. The sound is played at a
   location rather than from the coin itself, so it keeps playing after
   the coin is destroyed.

```
◆ OnTriggerEnter ──▶ Cast To BP_ThirdPersonCharacter ──success──▶ Play Sound at Location
                                                                    ──▶ Get Game Mode → Cast To BP_CoinGameMode
                                                                          ──▶ Add Coin
                                                                                ──▶ Destroy Entity (Self)
```

9. Click **Compile**. If a pin is wrong, the node turns red and the
   **Compiler Results** panel says what to fix and links to the node.
10. Drag `BP_Coin` into the level 10 times. Select a coin and change
    `SpinSpeed` in the Inspector; the field turns bold with a blue bar,
    which means that coin overrides the default.

**Checks Phases 9, 12 and 13:** Blueprint classes, components, variables,
instance overrides, trigger events, casting.

---

## Step 4: add input for a "dash"

1. **+ Add › Input › Input Action**, name it `IA_Dash`, value type **Bool**.
2. Open `IMC_Default` (from the template), add `IA_Dash`, bind it to
   **Left Shift** and **Gamepad Right Shoulder**, with the trigger
   **Pressed**.
3. Open `BP_ThirdPersonCharacter`. Right-click › **Event InputAction
   IA_Dash**.
4. From **Triggered**: **Do Once** → **Launch Character** (velocity =
   **Get Forward Vector** × 1200, override XY on) → **Delay** 1.0 s →
   back to the Do Once **Reset** pin.

```
◆ IA_Dash (Triggered) ──▶ Do Once ──▶ Launch Character ──▶ Delay (1.0) ──┐
                           ▲ Reset ◀───────────────────────────────────────┘
```

That's a dash with a 1-second cooldown. The **Delay** node is latent: the
graph pauses there and continues a second later, without blocking the
game.

**Checks Phases 10 and 12:** input actions, latent nodes.

---

## Step 5: count coins in a Game Mode

1. **+ Add › Blueprint Class**, parent **Game Mode**, name `BP_CoinGameMode`.
2. Add variables: `Coins` (int, 0) and `CoinsToWin` (int, 10).
3. Add an **Event Dispatcher** named `OnAllCoinsCollected`.
4. Add a function **Add Coin**:
   - **Set Coins** = `Coins + 1`
   - **Print String** "Coins: {Coins} / {CoinsToWin}" (use **Format Text**)
   - **Branch** on `Coins >= CoinsToWin` → true → **Call
     OnAllCoinsCollected**
5. In **World Settings** (tab next to the Inspector), set **Game Mode** to
   `BP_CoinGameMode`.

**Checks Phase 12:** functions, dispatchers, Format Text, the gameplay
framework's Game Mode.

---

## Step 6: open the door when all coins are collected

1. Make `BP_Door` (parent Entity) with a **Mesh Renderer** (`SM_Door`) and
   a **Box Collider**.
2. Add a **Timeline** node named `OpenTimeline`: length 1 s, one float
   track `Alpha` going from 0 to 1 with an ease curve.
3. In **Event BeginPlay**: **Get Game Mode** → **Cast To
   BP_CoinGameMode** → **Bind Event to OnAllCoinsCollected**, bound to a
   **Custom Event** `OpenDoor`.
4. `OpenDoor` → **OpenTimeline (Play)**. From **Update**: **Set Relative
   Location** = **Lerp** (closed position, closed + (0, 3, 0), `Alpha`).

```
◆ BeginPlay ──▶ Get Game Mode → Cast → Bind Event to OnAllCoinsCollected ◀── ◆ OpenDoor
◆ OpenDoor ──▶ OpenTimeline ▶Play      ──Update──▶ Set Relative Location ( Lerp(A, B, Alpha) )
```

5. Place `BP_Door` in front of the goal area.

**Checks Phase 12:** Timeline, binding to dispatchers, custom events.

---

## Step 7: the goal and a win message

1. Make `BP_Goal` with a **Box Collider** set as a trigger and a glowing
   material.
2. **OnTriggerEnter** → Cast to the character → **Print String** "You
   win!" (to screen, 5 s) → **Delay** 3 s → **Open Level** `Level_01`
   (restarts).

(Phase 18 will replace Print String with a real win screen widget.)

---

## Step 8: debug it

Press **Alt+P** and play. If a coin doesn't count:

1. Open `BP_Coin` while the game runs. In the toolbar's **Debug** dropdown,
   pick the coin instance you're walking toward.
2. Watch the wires: when you touch the coin, the exec wires light up along
   the path that actually ran. If the light stops at **Cast To**, the
   cast failed (maybe something other than the player touched it).
3. Click a node and press **F9** to add a breakpoint. The game pauses when
   it's reached, and hovering a pin shows its current value. **F10**
   steps to the next node.
4. The **Messages** panel lists any runtime warnings, such as touching an
   entity that was already destroyed, with a link to the node.

**Checks Phase 12:** Blueprint debugger, instance filter, wire
animation, breakpoints, runtime messages.

---

## Step 9 (optional): the same coin in Luau

For people who prefer text code: delete the spin logic from `BP_Coin`,
add a **Script** component, create `Content/Scripts/Spin.luau`, and
assign it:

```lua
--!strict
local Spin = {}
--@range 0 720
Spin.speed = 180

function Spin:OnUpdate(dt: number)
    self.entity:AddLocalRotation(Rotator.new(0, self.speed * dt, 0))
end

return Spin
```

`speed` appears in the Inspector automatically. While the game is running,
change `180` to `720` and save: the coins speed up immediately without
stopping play.

**Checks Phase 11:** Luau scripts, exposed variables, hot reload during
PIE.

---

## Step 10: share it

Until Phase 25 (packaging) lands, share the project folder (`Content/`,
`Config/`, `CoinRun.aproject`); `Saved/` and `Intermediate/` are
regenerated. After Phase 25: **Build › Package Project › Windows** makes a
standalone `CoinRun.exe`.

---

## M2 acceptance checklist

Every item must work exactly as described above:

- [ ] Project created from the Third Person template; PIE restores the
      level on stop
- [ ] Gizmos with snapping, grouping, undo of every step
- [ ] Blueprint class with components, instance-editable variables and
      visible overrides
- [ ] Trigger events, Cast To, Destroy Entity, Play Sound at Location
- [ ] Input Action with two bindings; Do Once and Delay work as a cooldown
- [ ] Game Mode set in World Settings; functions; Format Text; dispatchers
- [ ] Timeline node animating a door; Bind Event to a dispatcher
- [ ] Open Level restarts the game
- [ ] Debugger: instance filter, wire animation, breakpoints, pin values
- [ ] Luau script with an exposed variable and hot reload during PIE
