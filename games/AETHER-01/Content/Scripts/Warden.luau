-- Greybox Warden encounter using existing entity, attribute and script APIs.
-- Red floor plates mark the exact attack area before each ranged strike.
local Warden = {}

-- Where the armor shell and the exposed core sit, relative to the Warden
-- (not mission coordinates), so the encounter works wherever it is placed.
--@tooltip The armor shell's offset from the Warden (shown in phase 1).
Warden.armor_offset = vector.create(0, 0, 0)
--@tooltip The exposed core's offset from the Warden (shown from phase 3).
Warden.core_offset = vector.create(0, 0, 1.7)

function Warden:OnCreate()
    self.player = nil
    for _, entity in ipairs(world:EntitiesWith("Tags")) do
        for _, tag in ipairs(entity:Get("Tags").names) do
            if tag == "Player" then self.player = entity end
        end
    end
    local player = self.player
    local completed = player ~= nil and Attributes.Get(player, "MissionStage") >= 4
    Attributes.Define(self.entity, "Health", completed and 0 or 12, 0, 12)
    Attributes.Define(self.entity, "Phase", 1, 1, 4)
    Attributes.Define(self.entity, "AttackState", 0, 0, 1)
    self.timer, self.telegraph, self.phase, self.state = 1.5, false, 1, "idle"
    self.target = vector.create(0, 0, 0)
    self.markers, self.armor, self.core = {}, nil, nil
    for _, entity in ipairs(world:EntitiesWith("Tags")) do
        for _, tag in ipairs(entity:Get("Tags").names) do
            if tag == "WardenMarker" then table.insert(self.markers, entity) end
            if tag == "WardenArmor" then self.armor = entity end
            if tag == "WardenCore" then self.core = entity end
        end
    end
end

function Warden:HideMarkers()
    for _, marker in ipairs(self.markers) do marker:Get("Transform").position = vector.create(0, -100, 0) end
    Attributes.SetBase(self.entity, "AttackState", 0)
end

-- Whether the Warden can see Kael: the engine's line of sight (AI.CanSee) when
-- the host has one; without it the arena counts as open (it always sees him).
function Warden:Sees(player)
    if AI == nil or not AI.IsSightReady() then return true end
    return AI.CanSee(self.entity, player)
end

function Warden:OnUpdate(dt)
    self:Decide(dt)
    if AI ~= nil and AI.SetDebugState ~= nil then AI.SetDebugState(self.entity, self.state) end
end

function Warden:Decide(dt)
    local player = self.player
    if player == nil or not player:IsValid() then return end
    local health = Attributes.Get(self.entity, "Health")
    if health <= 0 then
        self.state = "defeated"
        self:HideMarkers()
        if self.armor ~= nil then self.armor:Get("Transform").position = vector.create(0, -100, 0) end
        if self.core ~= nil then self.core:Get("Transform").position = vector.create(0, -100, 0) end
        return
    end
    if Attributes.Get(player, "MissionStage") ~= 3 or Attributes.Get(player, "Health") <= 0 then
        self:HideMarkers()
        self.timer, self.telegraph, self.state = 1.5, false, "idle"
        return
    end
    self.phase = health > 9 and 1 or health > 6 and 2 or health > 3 and 3 or 4
    Attributes.SetBase(self.entity, "Phase", self.phase)
    local at = self.entity:Get("Transform").position
    if self.armor ~= nil then
        self.armor:Get("Transform").position = self.phase >= 2 and vector.create(0, -100, 0) or at + self.armor_offset
    end
    if self.core ~= nil then
        self.core:Get("Transform").position = self.phase >= 3 and at + self.core_offset or vector.create(0, -100, 0)
    end
    -- Loss of target: out of sight, a pending strike is cancelled and it waits
    -- to see Kael again (phases above still follow its health).
    if not self:Sees(player) then
        self:HideMarkers()
        self.timer, self.telegraph, self.state = 1.5, false, "lost"
        return
    end
    self.state = self.telegraph and "telegraph" or "wait"
    self.timer -= dt
    if self.timer > 0 then return end
    if not self.telegraph then
        self.target = player:Get("Transform").position
        self.telegraph = true
        self.timer = self.phase >= 4 and 0.7 or self.phase >= 2 and 1.0 or 1.5
        local radius = self.phase >= 4 and 4 or self.phase >= 2 and 3.25 or 2.5
        -- Select the model matching this phase's actual attack area.
        for _, marker in ipairs(self.markers) do
            local kind = Attributes.Get(marker, "MarkerPhase")
            marker:Get("Transform").position = kind == (self.phase >= 4 and 4 or self.phase >= 2 and 2 or 1)
                and vector.create(self.target.x, 0.035, self.target.z) or vector.create(0, -100, 0)
        end
        self.radius = radius
        self.state = "telegraph"
        Attributes.SetBase(self.entity, "AttackState", 1)
    else
        local p = player:Get("Transform").position
        if math.abs(p.x - self.target.x) <= self.radius and math.abs(p.z - self.target.z) <= self.radius then
            Attributes.SetBase(player, "Health", Attributes.Get(player, "Health") - 1)
            Attributes.SetBase(player, "DamageFlash", 1)
        end
        self:HideMarkers()
        self.telegraph, self.state = false, "wait"
        self.timer = self.phase >= 4 and 0.65 or 1.2
    end
end

return Warden
