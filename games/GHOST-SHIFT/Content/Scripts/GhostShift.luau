-- Ghost Shift: the player's script for the Willow House first playable (single player / host side).
-- First-person look and movement on the engine's Jolt character controller, the two starter tools
-- (Spectral Vacuum and UV Scrubber, as inventory items), the contract quest, the haunting director and
-- the company save. All game rules run on the fixed step (the project's 30 Hz simulation), so the
-- outcome doesn't depend on the render rate. The Prankster is Content/Scripts/Prankster.luau.
--
-- State lives in attributes (readable by tests and --report):
--   player: Battery, Tank, TankMax, ToolIndex (1 vacuum, 2 UV), Haunting (0..100), HauntingStage (0 Quiet ..
--           4 Critical), PatchesCleaned, WrongCleans, StainScrubbed, TankEmptied, ToolsRecovered, Stuns,
--           Extracted, Currency, ContractsCompleted, SaveStatus (1 saved/loaded, -1 failed)
--   each Ectoplasm patch: Residue (2 vacuum pulses each); the CursedStain: Residue (1 UV scrub)
local GhostShift = {}

--@range 0 1
--@tooltip Degrees of view turn per pixel of mouse movement
GhostShift.look_speed = 0.12
--@range 0.5 6
--@tooltip How far the tools reach, metres
GhostShift.reach = 2.5
--@range 1 12
--@tooltip How far the vacuum's stun blast reaches, metres
GhostShift.stun_range = 6
--@range 0.1 2
--@tooltip Seconds between tool pulses while the tool button is held
GhostShift.pulse_interval = 0.4

local Quest = "WillowCleanup"
local SaveSlot = "company"
local Tools = {"SpectralVacuum", "UVScrubber"}
local StageNames = {"Quiet", "Stirring", "Active", "Dangerous", "Critical"}
local FAR = vector.create(0, -100, 0)
local GhostStates = {Dormant = 0, Investigating = 1, Pranking = 2, Hunting = 3, Stunned = 4, Contained = 5}

local function TagsOf(entity)
    local ok, tags = pcall(function() return entity:Get("Tags").names end)
    return ok and tags or {}
end

local function Quat(yaw, pitch)
    local hy, hp = yaw / 2, pitch / 2
    return {
        math.cos(hy) * math.sin(hp), math.sin(hy) * math.cos(hp),
        -math.sin(hy) * math.sin(hp), math.cos(hy) * math.cos(hp),
    }
end

local function Play(entity)
    if entity ~= nil and entity:IsValid() then entity:Get("AudioSource"):Play() end
end

function GhostShift:OnCreate()
    local e = self.entity
    self.yaw, self.pitch = 0, 0
    self.character = self.entity:Get("CharacterMovement")
    self.camera = nil
    self.patches, self.sfx = {}, {}
    for _, entity in ipairs(world:EntitiesWith("Tags")) do
        for _, tag in ipairs(TagsOf(entity)) do
            if tag == "Ectoplasm" then
                table.insert(self.patches, entity)
                Attributes.Define(entity, "Residue", 2, 0, 2)
            elseif tag == "CursedStain" then
                self.stain = entity
                Attributes.Define(entity, "Residue", 1, 0, 1)
            elseif tag == "Van" then
                self.van = entity
            elseif tag == "StolenTool" then
                self.stolen_tool = entity
                Attributes.Define(entity, "Item", 0, 0, 2)
            elseif tag == "Prankster" then
                self.prankster = entity
            elseif tag == "MainCamera" then
                self.camera = entity
            elseif string.sub(tag, 1, 3) == "Sfx" then
                self.sfx[tag] = entity
            end
        end
    end

    Attributes.Define(e, "Battery", 30, 0, 30)
    Attributes.Define(e, "Tank", 0, 0, 6)
    Attributes.Define(e, "TankMax", 6, 0, 6)
    Attributes.Define(e, "ToolIndex", 1, 1, 2)
    Attributes.Define(e, "Haunting", 0, 0, 100)
    Attributes.Define(e, "HauntingStage", 0, 0, 4)
    for _, name in ipairs({"PatchesCleaned", "WrongCleans", "StainScrubbed", "TankEmptied", "ToolsRecovered", "Stuns", "Extracted", "VacuumPulses"}) do
        Attributes.Define(e, name, 0, 0, 1000)
    end
    Attributes.Define(e, "Currency", 0, 0, 1e9)
    Attributes.Define(e, "ContractsCompleted", 0, 0, 1e6)
    Attributes.Define(e, "SaveStatus", 0, -1, 1)
    Attributes.Define(e, "HasVacuum", 1, 0, 1)
    Attributes.Define(e, "HasScrubber", 1, 0, 1)
    Attributes.Define(e, "QuestDone", 0, 0, 1)

    Inventory.Configure(e, 4)
    for _, tool in ipairs(Tools) do Inventory.Add(e, tool, 1) end
    Abilities.Grant(e, "VacuumPulse")
    Abilities.Grant(e, "StunBlast")
    Quests.Start(e, Quest)
    self:LoadCompany()

    self.pulse_timer = 0
    self.interact_pending, self.switch_pending = false, false
    self.interact_was, self.switch_was = false, false
    self.last_stage = 0
    print("Ghost Shift: Willow House contract accepted - " .. #self.patches .. " ectoplasm patches, Prankster on site")
end

-- Company save --------------------------------------------------------------------------------------

function GhostShift:LoadCompany()
    if not SaveGames.DoesSaveExist(SaveSlot) then return end
    if not SaveGames.LoadFromSlot(SaveSlot) or SaveGames.GetNumber("schema", 0) ~= 1 then
        Attributes.SetBase(self.entity, "SaveStatus", -1)
        return
    end
    Attributes.SetBase(self.entity, "Currency", math.max(0, SaveGames.GetNumber("currency", 0)))
    Attributes.SetBase(self.entity, "ContractsCompleted", math.max(0, SaveGames.GetNumber("contracts_completed", 0)))
    Attributes.SetBase(self.entity, "SaveStatus", 1)
end

function GhostShift:SaveCompany()
    SaveGames.CreateSaveObject()
    SaveGames.SetNumber("schema", 1)
    SaveGames.SetNumber("currency", Attributes.GetBase(self.entity, "Currency"))
    SaveGames.SetNumber("contracts_completed", Attributes.GetBase(self.entity, "ContractsCompleted"))
    Attributes.SetBase(self.entity, "SaveStatus", SaveGames.SaveToSlot(SaveSlot) and 1 or -1)
end

function GhostShift:OnQuestCompleted(quest)
    if quest ~= Quest then return end
    -- The ContractPayout reward effect has paid the Currency; the quest system completes a quest once.
    Attributes.AddBase(self.entity, "ContractsCompleted", 1)
    self:SaveCompany()
    print("Ghost Shift: contract complete - balance " .. Attributes.GetBase(self.entity, "Currency"))
end

-- Helpers -------------------------------------------------------------------------------------------

function GhostShift:Position()
    return self.entity:Get("Transform").position
end

function GhostShift:HeldTool()
    return Tools[Attributes.Get(self.entity, "ToolIndex")]
end

function GhostShift:Nearest(list, range)
    local best, best_distance = nil, range
    local position = self:Position()
    for _, entity in ipairs(list) do
        if Attributes.Get(entity, "Residue") > 0 then
            local d = vector.magnitude(entity:Get("Transform").position - position)
            if d <= best_distance then best, best_distance = entity, d end
        end
    end
    return best
end

function GhostShift:AddHaunting(amount)
    Attributes.AddBase(self.entity, "Haunting", amount)
end

-- Tools ---------------------------------------------------------------------------------------------

function GhostShift:TryStun()
    local ghost = self.prankster
    if ghost == nil or not ghost:IsValid() then return false end
    if Attributes.Get(ghost, "GhostState") ~= GhostStates.Investigating then return false end
    if vector.magnitude(ghost:Get("Transform").position - self:Position()) > self.stun_range then return false end
    local handle = Abilities.TryActivate(self.entity, "StunBlast") -- costs Battery 4
    if handle == 0 then return false end
    Abilities.End(handle)
    Attributes.SetBase(ghost, "StunRequest", 1)
    Attributes.AddBase(self.entity, "Stuns", 1)
    Play(self.sfx.SfxStun)
    return true
end

function GhostShift:Vacuum()
    if self:TryStun() then return true end
    local patch = self:Nearest(self.patches, self.reach)
    if patch == nil then return false end -- nothing left in reach: never spend on a clean patch
    if Attributes.Get(self.entity, "Tank") >= Attributes.Get(self.entity, "TankMax") then return false end
    local handle = Abilities.TryActivate(self.entity, "VacuumPulse") -- Battery -2, Tank +1 (VacuumPulseCost)
    if handle == 0 then return false end
    Abilities.End(handle)
    Attributes.AddBase(patch, "Residue", -1)
    Attributes.AddBase(self.entity, "VacuumPulses", 1)
    self:AddHaunting(-2)
    Play(self.sfx.SfxVacuum)
    if Attributes.Get(patch, "Residue") <= 0 then
        patch:Get("Transform").position = FAR
        Attributes.AddBase(self.entity, "PatchesCleaned", 1)
        Quests.Progress(self.entity, Quest, "clean_ectoplasm", 1)
    end
    return true
end

function GhostShift:Scrub()
    -- The UV scrubber is for cursed stains. On ectoplasm it smears it around and angers the house.
    if self.stain ~= nil and self:Nearest({self.stain}, self.reach) ~= nil then
        Attributes.SetBase(self.stain, "Residue", 0)
        self.stain:Get("Transform").position = FAR
        Attributes.SetBase(self.entity, "StainScrubbed", 1)
        Quests.Progress(self.entity, Quest, "scrub_stain", 1)
        self:AddHaunting(-5)
        return true
    end
    if self:Nearest(self.patches, self.reach) ~= nil then
        Attributes.AddBase(self.entity, "WrongCleans", 1)
        self:AddHaunting(6)
        return true
    end
    return false
end

function GhostShift:Interact()
    local position = self:Position()
    -- Pick up a tool the Prankster hid (the hatch pole reaches the attic, so height counts up to 3.6 m).
    local tool = self.stolen_tool
    if tool ~= nil and Attributes.Get(tool, "Item") > 0 then
        local at = tool:Get("Transform").position
        local flat = vector.create(at.x - position.x, 0, at.z - position.z)
        if vector.magnitude(flat) <= 2 and math.abs(at.y - position.y) <= 3.6 then
            Inventory.Add(self.entity, Tools[Attributes.Get(tool, "Item")], 1)
            Attributes.SetBase(tool, "Item", 0)
            tool:Get("Transform").position = FAR
            Attributes.AddBase(self.entity, "ToolsRecovered", 1)
            return
        end
    end
    if self.van ~= nil and vector.magnitude(self.van:Get("Transform").position - position) <= 3.5 then
        local tank = Attributes.Get(self.entity, "Tank")
        if tank > 0 then
            Attributes.AddBase(self.entity, "TankEmptied", tank)
            Attributes.SetBase(self.entity, "Tank", 0)
        end
        if Quests.GetProgress(self.entity, Quest, "clean_ectoplasm") >= 4 and Attributes.Get(self.entity, "Extracted") == 0 then
            Attributes.SetBase(self.entity, "Extracted", 1)
            Quests.Progress(self.entity, Quest, "extract", 1)
        end
    end
end

-- Haunting director ---------------------------------------------------------------------------------

function GhostShift:UpdateHaunting(dt)
    local dirty = 0
    for _, patch in ipairs(self.patches) do
        if Attributes.Get(patch, "Residue") > 0 then dirty += 1 end
    end
    if dirty > 0 then self:AddHaunting(dt * 0.15 * dirty) else self:AddHaunting(-dt) end
    local stage = math.min(4, math.floor(Attributes.Get(self.entity, "Haunting") / 20))
    if stage ~= self.last_stage then
        self.last_stage = stage
        Attributes.SetBase(self.entity, "HauntingStage", stage)
        print("Ghost Shift: haunting " .. StageNames[stage + 1])
    end
end

-- Frame and fixed step ------------------------------------------------------------------------------

function GhostShift:OnUpdate(dt)
    -- Presses are latched every frame and consumed by the next fixed step, whatever the render rate.
    local interact, switch = Input.IsTriggered("Interact"), Input.IsTriggered("SwitchTool")
    if interact and not self.interact_was then self.interact_pending = true end
    if switch and not self.switch_was then self.switch_pending = true end
    self.interact_was, self.switch_was = interact, switch

    local look = Input.GetAxis2D("Look")
    self.yaw = self.yaw - math.rad(look.x * self.look_speed)
    self.pitch = math.clamp(self.pitch - math.rad(look.y * self.look_speed), -1.4, 1.4)
    local move = Input.GetAxis2D("Move")
    if vector.magnitude(move) > 1 then move = vector.normalize(move) end
    local sy, cy = math.sin(self.yaw), math.cos(self.yaw)
    self.character.input = vector.create(-sy * move.y + cy * move.x, 0, -cy * move.y - sy * move.x)
    self.entity:Get("Transform").rotation = {0, math.sin(self.yaw / 2), 0, math.cos(self.yaw / 2)}
    if self.camera ~= nil and self.camera:IsValid() then
        local camera = self.camera:Get("Transform")
        camera.position = self:Position() + vector.create(0, 1.6, 0)
        camera.rotation = Quat(self.yaw, self.pitch)
    end
end

function GhostShift:OnFixedUpdate(dt)
    if self.switch_pending then
        self.switch_pending = false
        Attributes.SetBase(self.entity, "ToolIndex", Attributes.Get(self.entity, "ToolIndex") == 1 and 2 or 1)
    end
    if self.interact_pending then
        self.interact_pending = false
        self:Interact()
    end
    self.pulse_timer = math.max(0, self.pulse_timer - dt)
    local held = self:HeldTool()
    if Input.IsTriggered("UseTool") and self.pulse_timer <= 0 and Inventory.Has(self.entity, held) then
        local used = false
        if held == "SpectralVacuum" then used = self:Vacuum() else used = self:Scrub() end
        if used then self.pulse_timer = self.pulse_interval end
    end
    self:UpdateHaunting(dt)
    -- Mirrors for tests and the end-of-run report.
    Attributes.SetBase(self.entity, "HasVacuum", Inventory.Has(self.entity, Tools[1]) and 1 or 0)
    Attributes.SetBase(self.entity, "HasScrubber", Inventory.Has(self.entity, Tools[2]) and 1 or 0)
    Attributes.SetBase(self.entity, "QuestDone", Quests.IsCompleted(self.entity, Quest) and 1 or 0)
end

function GhostShift:OnDestroy()
    -- Printed at the end of a run (aether_player --report prints the engine's own Player line before this).
    local names = {"Battery", "Tank", "ToolIndex", "Haunting", "HauntingStage", "PatchesCleaned", "WrongCleans",
                   "StainScrubbed", "TankEmptied", "ToolsRecovered", "Stuns", "Extracted", "Currency", "ContractsCompleted", "SaveStatus", "HasVacuum", "HasScrubber", "QuestDone"}
    local parts = {}
    for _, name in ipairs(names) do
        table.insert(parts, string.format("%q: %g", name, Attributes.Get(self.entity, name)))
    end
    print("Ghost Shift attributes: {" .. table.concat(parts, ", ") .. "}")
end

return GhostShift
