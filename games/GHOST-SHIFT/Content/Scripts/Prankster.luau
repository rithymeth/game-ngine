-- Ghost Shift: the Prankster, Willow House's resident ghost. It steals the tool you are holding and hides it.
-- Runs entirely on the fixed step with its own seeded generator, so a run is the same at any render rate.
--
-- States (GhostState attribute): 0 Dormant, 1 Investigating, 2 Pranking, 3 Hunting (not used by the
-- Prankster), 4 Stunned, 5 Contained (reserved for the containment trap).
--   Dormant -> Investigating: after a seeded wait that shortens as the haunting stage rises.
--   Investigating: drifts to a seeded waypoint. If it can steal something, it WARNS for 1.5 s first:
--     its light flares, its moan plays from where it is (spatial cue) and Warnings goes up by one.
--   Pranking (one step): takes the player's held tool (or the other one), hides it at a seeded hiding spot,
--     raises the haunting by 5, then goes Dormant again.
--   Counterplay: a vacuum stun blast (the player's StunBlast ability, Battery 4) while it is Investigating
--     (warning included) stuns it for 4 s and cancels the prank. Hidden tools are picked up with Interact.
-- Attributes for tests and debugging: GhostState, Warning, Warnings, Pranks, Stuns, LastHideSpot,
-- LastWarningStep, LastPrankStep, Step; Suppressed = 1 keeps it Dormant (a test hook).
local Prankster = {}

--@range 1 2147483646
--@tooltip Seed for the Prankster's choices (waits, waypoints, hiding spots)
Prankster.seed = 1337
--@range 0 10
--@tooltip Drift speed, metres per second
Prankster.speed = 1.6
--@range 0 5
--@tooltip Seconds of warning (light + moan) before every prank
Prankster.warning_time = 1.5
--@range 0 20
Prankster.investigate_time = 3
--@range 0 20
Prankster.stun_time = 4

local Tools = {"SpectralVacuum", "UVScrubber"}
local S = {Dormant = 0, Investigating = 1, Pranking = 2, Hunting = 3, Stunned = 4, Contained = 5}

local function TagsOf(entity)
    local ok, tags = pcall(function() return entity:Get("Tags").names end)
    return ok and tags or {}
end

function Prankster:Random()
    -- Park-Miller minimal standard generator: exact in double precision.
    self.rng = (self.rng * 48271) % 2147483647
    return self.rng / 2147483647
end

function Prankster:OnCreate()
    local e = self.entity
    self.rng = math.max(1, math.floor(self.seed) % 2147483647)
    self.waypoints, self.hiding = {}, {}
    for _, entity in ipairs(world:EntitiesWith("Tags")) do
        for _, tag in ipairs(TagsOf(entity)) do
            local index = tonumber(string.match(tag, "^Waypoint(%d+)$"))
            if index then self.waypoints[index] = entity:Get("Transform").position end
            index = tonumber(string.match(tag, "^Hiding(%d+)$"))
            if index then self.hiding[index] = entity:Get("Transform").position end
            if tag == "Player" then self.player = entity end
            if tag == "StolenTool" then self.tool = entity end
        end
    end
    for _, name in ipairs({"GhostState", "Warning", "Warnings", "Pranks", "Stuns", "LastHideSpot", "StunRequest", "Suppressed"}) do
        Attributes.Define(e, name, 0, 0, 1e6)
    end
    Attributes.Define(e, "LastWarningStep", -1, -1, 1e9)
    Attributes.Define(e, "LastPrankStep", -1, -1, 1e9)
    Attributes.Define(e, "Step", 0, 0, 1e9)
    self.step = 0
    self.light = e:Get("PointLight")
    self:EnterDormant()
end

function Prankster:SetState(state)
    self.state = state
    Attributes.SetBase(self.entity, "GhostState", state)
end

function Prankster:HauntingStage()
    if self.player == nil then return 0 end
    return Attributes.Get(self.player, "HauntingStage")
end

function Prankster:EnterDormant()
    self:SetState(S.Dormant)
    self.timer = (4 + 4 * self:Random()) * (1 - 0.1 * self:HauntingStage())
    self:SetWarning(false)
end

function Prankster:SetWarning(on)
    self.warning = on
    Attributes.SetBase(self.entity, "Warning", on and 1 or 0)
    self.light.intensity = on and 30 or 0
end

-- The tool it would steal: the held one, else the other; nil if the player has none or one is already hidden.
function Prankster:Target()
    if self.player == nil or self.tool == nil or Attributes.Get(self.tool, "Item") > 0 then return nil end
    local held = Attributes.Get(self.player, "ToolIndex")
    for _, index in ipairs({held, 3 - held}) do
        if Inventory.Has(self.player, Tools[index]) then return index end
    end
    return nil
end

function Prankster:Prank()
    local index = self:Target()
    if index == nil then return end
    Inventory.Remove(self.player, Tools[index], 1)
    local spot = 1 + math.floor(self:Random() * #self.hiding)
    Attributes.SetBase(self.tool, "Item", index)
    self.tool:Get("Transform").position = self.hiding[spot] + vector.create(0, 0.15, 0)
    Attributes.AddBase(self.player, "Haunting", 5)
    Attributes.AddBase(self.entity, "Pranks", 1)
    Attributes.SetBase(self.entity, "LastHideSpot", spot)
    Attributes.SetBase(self.entity, "LastPrankStep", self.step)
    print("Prankster: stole the " .. Tools[index] .. " and hid it (spot " .. spot .. ")")
end

function Prankster:OnFixedUpdate(dt)
    self.step += 1
    Attributes.SetBase(self.entity, "Step", self.step)
    if Attributes.Get(self.entity, "StunRequest") > 0 then
        Attributes.SetBase(self.entity, "StunRequest", 0)
        if self.state == S.Investigating then
            self:SetState(S.Stunned)
            self:SetWarning(false)
            self.timer = self.stun_time
            Attributes.AddBase(self.entity, "Stuns", 1)
            return
        end
    end
    if Attributes.Get(self.entity, "Suppressed") > 0 then return end
    self.timer -= dt
    if self.state == S.Dormant then
        if self.timer <= 0 then
            self:SetState(S.Investigating)
            self.timer = self.investigate_time
            self.goal = self.waypoints[1 + math.floor(self:Random() * #self.waypoints)]
        end
    elseif self.state == S.Investigating then
        if not self.warning then
            local transform = self.entity:Get("Transform")
            local to = self.goal - transform.position
            local distance = vector.magnitude(to)
            if distance > 0.01 then
                transform.position = transform.position + to * (math.min(distance, self.speed * dt) / distance)
            end
            if self.timer <= 0 then
                if self:Target() ~= nil then
                    -- Every prank is announced: light, spatial moan, and the Warnings count.
                    self:SetWarning(true)
                    self.timer = self.warning_time
                    Attributes.AddBase(self.entity, "Warnings", 1)
                    Attributes.SetBase(self.entity, "LastWarningStep", self.step)
                    self.entity:Get("AudioSource"):Play()
                else
                    self:EnterDormant()
                end
            end
        elseif self.timer <= 0 then
            self:SetState(S.Pranking)
            self:Prank()
            self:EnterDormant()
        end
    elseif self.state == S.Stunned then
        if self.timer <= 0 then self:EnterDormant() end
    end
end

return Prankster
