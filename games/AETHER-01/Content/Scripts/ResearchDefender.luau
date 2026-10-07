-- The service route's shielded Sentinel and ambushing Hunter are game content.
local Defender = {}

local function ClearSight(shooter, target)
    -- Same unrotated-box contract as the game's hitscan/camera queries.
    local available, boxes = pcall(function() return world:EntitiesWith("BoxCollider") end)
    if not available then return true end
    local origin = shooter:Get("Transform").position
    local endpoint = target:Get("Transform").position + vector.create(0, 1, 0)
    local delta = endpoint - origin
    for _, box in ipairs(boxes) do
        if box ~= shooter and box ~= target then
            local collider = box:Get("BoxCollider")
            if not collider.is_trigger then
                local center = box:Get("Transform").position + collider.center
                local first, last = 0, 1
                for _, axis in ipairs({"x", "y", "z"}) do
                    local low, high = center[axis] - collider.half_extents[axis], center[axis] + collider.half_extents[axis]
                    if math.abs(delta[axis]) < 0.00001 then
                        if origin[axis] < low or origin[axis] > high then first, last = 1, 0; break end
                    else
                        local a, b = (low - origin[axis]) / delta[axis], (high - origin[axis]) / delta[axis]
                        if a > b then a, b = b, a end
                        first, last = math.max(first, a), math.min(last, b)
                    end
                end
                if first <= last then return false end
            end
        end
    end
    return true
end

function Defender:OnCreate()
    self.player, self.marker, self.shell = nil, nil, nil
    self.hunter = false
    for _, tag in ipairs(self.entity:Get("Tags").names) do
        if tag == "Hunter" then self.hunter = true end
    end
    for _, entity in ipairs(world:EntitiesWith("Tags")) do
        for _, tag in ipairs(entity:Get("Tags").names) do
            if tag == "Player" then self.player = entity end
            if tag == (self.hunter and "HunterTell" or "SentinelAim") then self.marker = entity end
            if tag == "SentinelShield" and not self.hunter then self.shell = entity end
        end
    end
    local complete = self.player ~= nil and Attributes.Get(self.player, "MissionStage") >= 3
    local maximum = self.hunter and 4 or 6
    Attributes.Define(self.entity, "Health", complete and 0 or maximum, 0, maximum)
    if not self.hunter then Attributes.Define(self.entity, "Shield", complete and 0 or 6, 0, 6) end
    Attributes.Define(self.entity, "AttackState", 0, 0, 1)
    Attributes.Define(self.entity, "Stagger", 0, 0, 1)
    Attributes.Define(self.entity, "KnockbackScale", self.hunter and 1 or 0.15, 0, 1)
    self.state, self.timer, self.damaged = "idle", 2, false
    self.target = vector.create(0, 0, 0)
end

function Defender:HideTell()
    if self.marker ~= nil then self.marker:Get("Transform").position = vector.create(0, -100, 0) end
    Attributes.SetBase(self.entity, "AttackState", 0)
end

function Defender:DamagePlayer()
    if not ClearSight(self.entity, self.player) then return end
    Attributes.SetBase(self.player, "Health", Attributes.Get(self.player, "Health") - 1)
    Attributes.SetBase(self.player, "DamageFlash", 1)
end

function Defender:OnUpdate(dt)
    if self.player == nil or not self.player:IsValid() then return end
    local health = Attributes.Get(self.entity, "Health")
    local transform = self.entity:Get("Transform")
    if self.shell ~= nil then
        self.shell:Get("Transform").position = health > 0 and Attributes.Get(self.entity, "Shield") > 0
            and transform.position or vector.create(0, -100, 0)
    end
    if health <= 0 or Attributes.Get(self.player, "MissionStage") ~= 2 or Attributes.Get(self.player, "Health") <= 0 then
        self:HideTell()
        self.state, self.timer = "idle", 2
        return
    end
    local stagger = Attributes.Get(self.entity, "Stagger")
    if stagger > 0 then
        Attributes.SetBase(self.entity, "Stagger", math.max(0, stagger - dt))
        self:HideTell()
        self.state, self.timer = "idle", 1
        return
    end
    local player = self.player:Get("Transform").position
    local p = transform.position
    local dx, dz = player.x - p.x, player.z - p.z
    local distance = math.sqrt(dx * dx + dz * dz)
    self.timer -= dt
    if self.hunter then
        if player.z > -13 then return end -- ambush stays dormant until Kael enters research
        if self.state == "charge" then
            local cx, cz = self.target.x - p.x, self.target.z - p.z
            local length = math.sqrt(cx * cx + cz * cz)
            if length > 0.05 then
                local step = math.min(8 * dt, length)
                transform.position = vector.create(math.clamp(p.x + cx / length * step, -4.3, 4.3),
                                                    p.y, math.clamp(p.z + cz / length * step, -21.5, -9))
            end
            if distance < 1.4 and not self.damaged then self:DamagePlayer(); self.damaged = true end
            if self.timer <= 0 then self.state, self.timer = "idle", 1.5; self:HideTell() end
        elseif self.state == "tell" then
            if self.timer <= 0 then self.state, self.timer, self.damaged = "charge", 0.65, false end
        elseif distance > 5 then
            local step = math.min(3.6 * dt, distance - 3)
            transform.position = vector.create(math.clamp(p.x + dx / distance * step, -4.3, 4.3),
                                                p.y, math.clamp(p.z + dz / distance * step, -21.5, -9))
        elseif self.timer <= 0 then
            self.state, self.timer, self.target = "tell", 0.75, player
            Attributes.SetBase(self.entity, "AttackState", 1)
            if self.marker ~= nil then self.marker:Get("Transform").position = vector.create(player.x, 0.035, player.z) end
        end
    elseif distance <= 18 and ClearSight(self.entity, self.player) then
        if self.state == "tell" and self.timer <= 0 then
            if math.abs(player.x - self.target.x) <= 0.8 and math.abs(player.z - self.target.z) <= 0.8 then self:DamagePlayer() end
            self.state, self.timer = "idle", 2
            self:HideTell()
        elseif self.state == "idle" and self.timer <= 0 then
            self.state, self.timer, self.target = "tell", 1.1, player
            Attributes.SetBase(self.entity, "AttackState", 1)
            if self.marker ~= nil then self.marker:Get("Transform").position = vector.create(player.x, 0.035, player.z) end
        end
    else
        self:HideTell()
        self.state, self.timer = "idle", 2
    end
end

return Defender
