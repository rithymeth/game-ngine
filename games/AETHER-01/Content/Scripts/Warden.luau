-- Greybox Warden encounter using existing entity, attribute and script APIs.
-- Red floor plates mark the exact attack area before each ranged strike.
local Warden = {}

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
    self.timer, self.telegraph, self.phase = 1.5, false, 1
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

function Warden:OnUpdate(dt)
    local player = self.player
    if player == nil or not player:IsValid() then return end
    local health = Attributes.Get(self.entity, "Health")
    if health <= 0 then
        self:HideMarkers()
        if self.armor ~= nil then self.armor:Get("Transform").position = vector.create(0, -100, 0) end
        if self.core ~= nil then self.core:Get("Transform").position = vector.create(0, -100, 0) end
        return
    end
    if Attributes.Get(player, "MissionStage") ~= 3 or Attributes.Get(player, "Health") <= 0 then
        self:HideMarkers()
        self.timer, self.telegraph = 1.5, false
        return
    end
    self.phase = health > 9 and 1 or health > 6 and 2 or health > 3 and 3 or 4
    Attributes.SetBase(self.entity, "Phase", self.phase)
    if self.armor ~= nil then
        self.armor:Get("Transform").position = self.phase >= 2 and vector.create(0, -100, 0) or vector.create(0, 2, -35)
    end
    if self.core ~= nil then
        self.core:Get("Transform").position = self.phase >= 3 and vector.create(0, 2, -33.3) or vector.create(0, -100, 0)
    end
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
        Attributes.SetBase(self.entity, "AttackState", 1)
    else
        local p = player:Get("Transform").position
        if math.abs(p.x - self.target.x) <= self.radius and math.abs(p.z - self.target.z) <= self.radius then
            Attributes.SetBase(player, "Health", Attributes.Get(player, "Health") - 1)
            Attributes.SetBase(player, "DamageFlash", 1)
        end
        self:HideMarkers()
        self.telegraph = false
        self.timer = self.phase >= 4 and 0.65 or 1.2
    end
end

return Warden
