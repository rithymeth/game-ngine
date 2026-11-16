-- Kael's hands (put it on his body, a child of the character: the character has the controller): the nearest thing tagged Salvage within reach can be taken with the Interact action (E, or X on a pad).
-- Taking it removes it from the world and adds what it gives to the pack. The pack is Kael's Inventory (Scrap, Component, Sample): the engine carries
-- it through a scene change (a door), and every change saves it (SaveGames.CaptureWorld, then the "autosave" slot) so the next run picks up where this one left off.
local Interactor = {}

--@range 0.5 8
--@tooltip How far Kael can reach, metres
Interactor.reach = 3
--@tooltip Components already in the pack at the start (for test scenes)
Interactor.start_components = 0
--@tooltip Scrap already in the pack at the start (for test scenes)
Interactor.start_scrap = 0
--@tooltip Seed samples already in the pack at the start (for test scenes)
Interactor.start_samples = 0

-- What a salvage entity gives, by the words in its name. Anything else gives scrap.
local YIELDS = {
    { "Sample", "samples", 1 },
    { "Crate", "components", 2 },
    { "Pile", "scrap", 3 },
    { "Wreck", "components", 5 },
}

-- The pack's items, by the word the scripts use for them. Kael (the entity with the controller) holds the Inventory.
local ITEM = { scrap = "Scrap", components = "Component", samples = "Sample" }
local ALL_ITEMS = { "Scrap", "Component", "Sample", "GeneratorCore" }
local owner = nil

local function Get(kind) return owner ~= nil and Inventory.Count(owner, ITEM[kind]) or 0 end

-- The pack is saved with Kael: capture him into the bag, then write the slot.
local function Persist()
    SaveGames.CaptureWorld()
    SaveGames.SaveToSlot("autosave")
end

local function Give(kind, amount)
    if owner ~= nil then Inventory.Add(owner, ITEM[kind], amount) end
    Persist()
end

-- Taken pickups stay taken: "taken.<scene>.<name>" in SaveGames (so they do not come back after a door or a restart, and F9 clears them).
local function TakenKey(entity)
    return "taken." .. Scene.Current() .. "." .. entity:Get("EntityName").value
end

local function IsPickup(entity)
    for _, t in ipairs(entity:Get("Tags").names) do
        if t == "Salvage" or t == "Sample" then return true end
    end
    return false
end

local function Sfx(name)
    if not Audio.PlaySound2D("Audio/" .. name .. ".acue") then print("Missing sound: " .. name) end
end

-- What the HUD calls each checkpoint (the names are the Checkpoint components' in the scenes).
local CHECKPOINT_TITLES = {
    Camp = "Crash Site Camp", Outpost = "Outpost", DistressRelay = "Distress Relay", WardenCheckpoint = "Warden Checkpoint",
    LivingDome = "Living Dome", CoreApproach = "Core Approach", Start = "Start",
}

-- A line for the HUD's toast panel: they queue, and each shows for a few seconds.
local function Toast(self, text)
    self.toasts[#self.toasts + 1] = text
end

-- Puts Kael at the last checkpoint (Checkpoints.Respawn: the engine's character controller takes the new position as a teleport).
local function RespawnAtCheckpoint(self)
    SaveGames.SetNumber("respawn.pending", 0)
    local last = Checkpoints.Last()
    local result = self.player ~= nil and Checkpoints.Respawn(self.player) or "no_checkpoint"
    if result == "ok" then
        print("Kael respawned at " .. last)
        self.verify = 3 -- a few frames on, say where he is (the check that the controller took it)
        Persist()
    else
        print("Kael has no checkpoint to return to here: " .. result)
    end
end

function Interactor:OnCreate()
    self.attack_cool = 0
    self.toasts = {}
    self.toast_left = 0
    self.arrived = false
    if not SaveGames.HasKey("player.hp") then SaveGames.SetNumber("player.hp", 100) end
    self.player = world:EntitiesWith("CharacterMovement")[1]
    owner = self.player
    -- the first scene of a run: pick the saved game up (the bag, then Kael's pack with it); a later scene finds the pack already on Kael
    if not SaveGames.HasKey("pack.init") and SaveGames.DoesSaveExist("autosave") then
        SaveGames.LoadFromSlot("autosave")
        if SaveGames.HasKey("pack.init") then SaveGames.RestoreWorld() end
    end
    if not SaveGames.HasKey("pack.init") then
        SaveGames.SetNumber("pack.init", 1)
        if owner ~= nil then
            if self.start_scrap > 0 then Inventory.Add(owner, "Scrap", self.start_scrap) end
            if self.start_samples > 0 then Inventory.Add(owner, "Sample", self.start_samples) end
            if self.start_components > 0 then Inventory.Add(owner, "Component", self.start_components) end
        end
    end
    print(string.format("Pack at start: %d scrap, %d components", Get("scrap"), Get("components")))
    self.seen_checkpoint = Checkpoints.Last() -- the one carried in (a loaded game, the last scene) is not news; a new one is
    print(string.format("Checkpoint at start: %s, achievements: %d unlocked", self.seen_checkpoint == "" and "none" or self.seen_checkpoint, Achievements.UnlockedCount()))
    self.hud_ready = false
    self.nearest = nil
    self.shown = nil
end

local function Yield(name)
    for _, y in ipairs(YIELDS) do
        if string.find(name, y[1], 1, true) then return y[2], y[3] end
    end
    return "scrap", 1
end

-- The mission line: what to do next, from what has been done (all of it in SaveGames, so it survives a door and a restart).
local function Objective()
    local function done(key) return SaveGames.GetNumber(key, 0) > 0 end
    if not done("crafted.generator_core") then return "Salvage scrap and components, craft a Generator Core." end
    if not done("power.outpost") then return "Install the Generator Core at the generator." end
    if not done("mule.repaired") then return "Repair MULE-3 in the Fallen Basin wreck." end
    if not done("mission.relay") then return "Take MULE-3 to the Distress Relay." end
    if not done("mission.dome") then
        if Get("samples") >= 3 then return "Take the seed samples to the console in the living dome." end
        return string.format("Chapter 2: collect three seed samples in the Verdant Array (%d of 3).", Get("samples"))
    end
    -- chapter 3: the two warden sentinels at the Iron Wastes checkpoint, then the assembly hall's terminal
    local down = 0
    for _, name in ipairs({ "WardenSentinel_0", "WardenSentinel_1" }) do
        if SaveGames.GetNumber("enemy." .. name .. ".hp", 1) <= 0 then down = down + 1 end
    end
    if down < 2 and not done("mission.assembly") then
        return string.format("Chapter 3: disable the two warden sentinels at the Iron Wastes checkpoint (%d of 2).", down)
    end
    if not done("mission.assembly") then return "Start the assembly line at the terminal in the assembly hall." end
    return "Chapter 3 complete. The assembly line runs."
end

-- Everything the first chapter saves, cleared for a new game (F9).
local function NewGame()
    SaveGames.CreateSaveObject() -- the whole bag: the chapter, Kael's health and every sentinel's
    SaveGames.DeleteSave("autosave")
    if owner ~= nil then -- and the pack, which the scene change would otherwise carry into the new game
        for _, item in ipairs(ALL_ITEMS) do
            local have = Inventory.Count(owner, item)
            if have > 0 then Inventory.Remove(owner, item, have) end
        end
    end
    print("New game: progress cleared")
    Scene.Load("Scenes/OpenWorld.ascene")
end

-- Kael's strike: every sentinel within reach loses health (kept in SaveGames, where the sentinel reads it).
local ATTACK_REACH = 2.6
local ATTACK_DAMAGE = 25

function Interactor:OnUpdate(dt)
    if Input.IsTriggered("NewGame") then return NewGame() end
    if self.shatter_t ~= nil then
        self.shatter_t = self.shatter_t - dt
        if self.shatter_t <= 0 then
            self.shatter_t = nil
            print(string.format("Crate shattered: %d pieces on the ground", Destruction.Debris()))
        end
    end
    local hp = SaveGames.GetNumber("player.hp", 100)
    if hp <= 0 then
        print("Kael disabled: back to the last checkpoint, his health restored")
        SaveGames.SetNumber("player.hp", 100)
        if Checkpoints.Last() ~= "" then
            local scene = Checkpoints.GetScene()
            if scene ~= "" and scene ~= Scene.Current() then
                SaveGames.SetNumber("respawn.pending", 1) -- the checkpoint is in another scene: load it, and the first frame there puts Kael on it
                Persist()
                return Scene.Load(scene)
            end
            RespawnAtCheckpoint(self)
            return
        end
        Persist() -- no checkpoint reached yet: back to the start of the world, as before
        return Scene.Load("Scenes/OpenWorld.ascene")
    end
    if not self.arrived then
        self.arrived = true
        if SaveGames.GetNumber("respawn.pending", 0) > 0 then RespawnAtCheckpoint(self) end
    end
    if self.verify ~= nil and self.verify > 0 and self.player ~= nil then
        self.verify = self.verify - 1
        if self.verify == 0 then
            local p = self.player:Get("Transform").position
            print(string.format("Kael after respawn at (%.1f, %.1f)", p.x, p.z))
        end
    end
    -- what the other scripts unlocked and what Kael reached: a line each for the HUD
    local name = Achievements.NextUnlocked()
    while name ~= "" do
        Toast(self, "ACHIEVEMENT  " .. Achievements.GetTitle(name))
        print("Achievement unlocked: " .. name)
        name = Achievements.NextUnlocked()
    end
    local last = Checkpoints.Last()
    if last ~= self.seen_checkpoint then
        self.seen_checkpoint = last
        if last ~= "" then
            Toast(self, "CHECKPOINT  " .. (CHECKPOINT_TITLES[last] or last))
            print("Checkpoint reached: " .. last)
            Persist()
        end
    end
    if self.toast_left > 0 then
        self.toast_left = self.toast_left - dt
        if self.toast_left <= 0 then UI.SetVisible("ToastPanel", false) end
    elseif #self.toasts > 0 and UI.SetVisible("ToastPanel", true) then
        UI.SetText("ToastText", table.remove(self.toasts, 1))
        self.toast_left = 3.5
    end
    UI.SetText("HealthText", string.format("HEALTH  %d", hp))
    self.attack_cool = self.attack_cool - dt
    if self.player ~= nil and self.attack_cool <= 0 and Input.IsTriggered("Attack") then
        self.attack_cool = 0.5
        Sfx("swing")
        local here = self.player:Get("Transform").position
        for _, e in ipairs(world:EntitiesWith("Tags")) do
            local isEnemy = false
            for _, t in ipairs(e:Get("Tags").names) do if t == "Enemy" then isEnemy = true end end
            if isEnemy then
                local p = e:Get("Transform").position
                local dx, dz = p.x - here.x, p.z - here.z
                if dx * dx + dz * dz <= ATTACK_REACH * ATTACK_REACH then
                    local key = "enemy." .. e:Get("EntityName").value .. ".hp"
                    SaveGames.SetNumber(key, SaveGames.GetNumber(key, 100) - ATTACK_DAMAGE)
                    Camera.Shake(0.15)
                    Sfx("hit_enemy")
                    print("Kael hit " .. e:Get("EntityName").value)
                end
            end
        end
    end
    if not self.hud_ready then -- the HUD exists from the first frame: show the pack as it is (it may be carried over)
        self.hud_ready = UI.SetText("ScrapText", string.format("SCRAP  %d", Get("scrap")))
        UI.SetText("ComponentText", string.format("COMPONENTS  %d", Get("components")))
        UI.SetText("SamplesText", string.format("SEED SAMPLES  %d / 3", Get("samples")))
    end
    local objective = Objective()
    if objective ~= self.objective and UI.SetText("ObjectiveText", objective) then self.objective = objective end
    if self.player == nil then return end
    local here = self.player:Get("Transform").position
    local best, best_d2 = nil, self.reach * self.reach
    for _, e in ipairs(world:EntitiesWith("Tags")) do
        local tags = e:Get("Tags").names
        if IsPickup(e) and SaveGames.HasKey(TakenKey(e)) then
            world:Destroy(e) -- taken before (an earlier visit or run): it does not come back
        elseif IsPickup(e) then
            local p = e:Get("Transform").position
            local dx, dz = p.x - here.x, p.z - here.z
            local d2 = dx * dx + dz * dz
            if d2 < best_d2 then best, best_d2 = e, d2 end
        end
    end
    self.nearest = best
    -- the prompt follows what is in reach
    local shown = best ~= nil and best:Get("EntityName").value or nil
    if shown ~= self.shown then
        -- the HUD exists from the first frame on: until then the call finds nothing and is tried again
        if UI.SetVisible("PromptPanel", shown ~= nil) then
            self.shown = shown
            if shown ~= nil then
                local kind, amount = Yield(shown)
                UI.SetText("PromptText", string.format("[E] Take  (+%d %s)", amount, kind))
            end
        end
    end
    if best ~= nil and Input.IsTriggered("Interact") then
        local name = best:Get("EntityName").value
        local kind, amount = Yield(name)
        SaveGames.SetNumber(TakenKey(best), 1)
        if kind ~= "samples" then
            Achievements.Add("FirstHaul")
            Achievements.Add("Scavenger") -- counts every piece of salvage; the unlock is saved with the pack just below
        end
        Give(kind, amount)
        if best:Get("Destructible") ~= nil then
            -- a crate breaks up rather than vanishing: the Destruction kit turns it into pieces at the next physics step, and `shatter_t` waits for them
            -- before saying so
            Destruction.Shatter(best)
            self.shatter_t = 0.4
        else
            world:Destroy(best)
        end
        Sfx("pickup")
        self.nearest = nil
        self.shown = nil
        UI.SetVisible("PromptPanel", false)
        UI.SetText("ScrapText", string.format("SCRAP  %d", Get("scrap")))
        UI.SetText("ComponentText", string.format("COMPONENTS  %d", Get("components")))
        if kind == "samples" then
            UI.SetText("SamplesText", string.format("SEED SAMPLES  %d / 3", Get("samples")))
            print(string.format("Collected seed sample %s (%d of 3)", name, Get("samples")))
        else
            print(string.format("Salvaged %s: +%d %s (pack: %d scrap, %d components)", name, amount, kind, Get("scrap"), Get("components")))
        end
    end
end

return Interactor
