-- The service route's shielded Sentinel and ambushing Hunter are game content.
local Defender = {}

-- Tuning. Where an archetype may move and when it wakes are per-instance data:
-- the scene overrides these on the Hunter (its room and its ambush trigger),
-- so no mission coordinates live in this script. The defaults bound nothing.
--@range 0 100
Defender.sight_range = 18
--@range 0 20
Defender.approach_speed = 3.6
--@range 0 40
Defender.charge_speed = 8
--@tooltip The Hunter keeps inside this box (world space, min corner).
Defender.arena_min = vector.create(-1e6, -1e6, -1e6)
--@tooltip The Hunter keeps inside this box (world space, max corner).
Defender.arena_max = vector.create(1e6, 1e6, 1e6)
--@tooltip The Hunter stays dormant until the player is inside this box (min corner).
Defender.wake_min = vector.create(-1e6, -1e6, -1e6)
--@tooltip The Hunter stays dormant until the player is inside this box (max corner).
Defender.wake_max = vector.create(1e6, 1e6, 1e6)

local function Inside(point, low, high)
    return point.x >= low.x and point.x <= high.x and point.y >= low.y and point.y <= high.y
        and point.z >= low.z and point.z <= high.z
end

local function BoxSight(shooter, target)
    -- Fallback for hosts without a perception world: the same unrotated-box
    -- contract as the game's hitscan/camera queries.
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

-- The engine's line of sight (AI.CanSee through the active PerceptionWorld)
-- when the host provides one; the script's own box test otherwise.
local function ClearSight(shooter, target)
    if AI ~= nil and AI.IsSightReady() then return AI.CanSee(shooter, target) end
    return BoxSight(shooter, target)
end

-- Gives the archetype an AIPerception (so AI.GetKnown / AI.GetTarget and the
-- AI debugger see what it senses) and makes Kael sensable. Its eyes are where
-- AI.CanSee already looked from (1.6 above the origin), so sight timing is
-- unchanged. Hosts without the AI module skip this.
local function GiveSenses(entity, player, range)
    pcall(function()
        local senses = entity:Add("AIPerception")
        senses.eye_height = 1.6
        senses.sight_radius = range
        senses.lose_sight_radius = range
        senses.fov_degrees = 360
        if player ~= nil and not player:Has("AIStimuliSource") then player:Add("AIStimuliSource") end
    end)
end

-- What the AI debugger shows for an entity (hosts without it skip this).
local function ReportState(entity, state)
    if AI ~= nil and AI.SetDebugState ~= nil then AI.SetDebugState(entity, state) end
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
    GiveSenses(self.entity, self.player, self.sight_range)
end

function Defender:Confine(x, y, z)
    local low, high = self.arena_min, self.arena_max
    return vector.create(math.clamp(x, low.x, high.x), y, math.clamp(z, low.z, high.z))
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
    self:Decide(dt)
    ReportState(self.entity, self.state)
end

function Defender:Decide(dt)
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
        if not Inside(player, self.wake_min, self.wake_max) then
            -- The ambush waits for its trigger; a pending tell is dropped when the
            -- player leaves it (a committed charge still plays out).
            if self.state == "tell" then self:HideTell(); self.state, self.timer = "idle", 1.5 end
            if self.state ~= "charge" then return end
        end
        if self.state == "charge" then
            local cx, cz = self.target.x - p.x, self.target.z - p.z
            local length = math.sqrt(cx * cx + cz * cz)
            if length > 0.05 then
                local step = math.min(self.charge_speed * dt, length)
                transform.position = self:Confine(p.x + cx / length * step, p.y, p.z + cz / length * step)
            end
            if distance < 1.4 and not self.damaged then self:DamagePlayer(); self.damaged = true end
            if self.timer <= 0 then self.state, self.timer = "idle", 1.5; self:HideTell() end
        elseif self.state == "tell" then
            if self.timer <= 0 then self.state, self.timer, self.damaged = "charge", 0.65, false end
        elseif distance > 5 then
            local step = math.min(self.approach_speed * dt, distance - 3)
            -- Approach along the navigation mesh (around props and obstacles)
            -- when the host has one; straight at the player otherwise.
            local moved = false
            if Nav ~= nil and Nav.IsReady() and dt > 0 then
                local arrived, remaining = Nav.MoveTowards(self.entity, player, step / dt, dt)
                if remaining ~= nil and (arrived or remaining > 0.05) then
                    local at = transform.position
                    transform.position = self:Confine(at.x, p.y, at.z)
                    moved = true
                end
            end
            if not moved then
                transform.position = self:Confine(p.x + dx / distance * step, p.y, p.z + dz / distance * step)
            end
        elseif self.timer <= 0 then
            self.state, self.timer, self.target = "tell", 0.75, player
            Attributes.SetBase(self.entity, "AttackState", 1)
            if self.marker ~= nil then self.marker:Get("Transform").position = vector.create(player.x, 0.035, player.z) end
        end
    elseif distance <= self.sight_range and ClearSight(self.entity, self.player) then
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
