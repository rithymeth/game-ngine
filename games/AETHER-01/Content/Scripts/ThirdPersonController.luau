-- Third person controller: put it on the character (tagged Player). The Look
-- axis orbits the camera (the entity with a Camera component) around it, WASD or
-- the left stick moves relative to where the camera looks, and the character
-- turns to face where it goes. Space or the A button jumps.
local Controller = {}

local function SegmentIntersectsBox(origin, endpoint, center, half_extents)
    local delta = endpoint - origin
    local min_t, max_t = 0, 1
    for _, axis in ipairs({"x", "y", "z"}) do
        local o, d = origin[axis], delta[axis]
        local low = center[axis] - half_extents[axis]
        local high = center[axis] + half_extents[axis]
        if math.abs(d) < 0.00001 then
            if o < low or o > high then return false end
        else
            local a, b = (low - o) / d, (high - o) / d
            if a > b then a, b = b, a end
            min_t, max_t = math.max(min_t, a), math.min(max_t, b)
            if min_t > max_t then return false end
        end
    end
    return true
end

local function HasClearShot(origin, endpoint, shooter, target)
    -- The current HELIOS-7 greybox uses unrotated BoxColliders. Keep this
    -- query game-owned until a level needs additional collider shapes.
    for _, obstacle in ipairs(world:EntitiesWith("BoxCollider")) do
        if obstacle:IsValid() and obstacle ~= shooter and obstacle ~= target then
            local collider = obstacle:Get("BoxCollider")
            if not collider.is_trigger then
                local transform = obstacle:Get("Transform")
                local center = transform.position + collider.center
                if SegmentIntersectsBox(origin, endpoint, center, collider.half_extents) then
                    return false
                end
            end
        end
    end
    return true
end

--@range 0 30
--@tooltip Running speed, metres per second
Controller.speed = 6
--@range 0 1
Controller.look_speed = 0.12
--@range 1 20
--@tooltip How far the camera sits behind the character
Controller.distance = 6
--@range 0 5
--@tooltip How high above the feet the camera looks
Controller.height = 1.6
--@range 0 20
Controller.jump_speed = 5.5
--@range -60 0
Controller.gravity = -15
--@range 0 100
--@tooltip Distance at which the AEGIS rifle can hit a Scout
Controller.weapon_range = 24
--@range 0 10
Controller.fire_cooldown = 0.22

function Controller:OnCreate()
    self.yaw = 0
    self.pitch = -0.3
    self.vy = 0
    self.floor = self.entity:Get("Transform").position.y
    self.camera = world:EntitiesWith("Camera")[1]
    self.character = self.entity:Get("CharacterMovement")
    self.fire_timer = 0
    self.scout_health = {}
    self.scout_spawn = {}
    self.spawn_position = self.entity:Get("Transform").position
    self.health = 3
    self.ammo_capacity = 30
    self.ammo = self.ammo_capacity
    self.reloading = false
    self.reload_timer = 0
    self.damage_flash = 0
    self.hit_confirm = 0
    self.damage_timer = 0
    self.dead = false
    Attributes.Define(self.entity, "Health", self.health, 0, 3)
    Attributes.Define(self.entity, "Ammo", self.ammo, 0, self.ammo_capacity)
    Attributes.Define(self.entity, "Reloading", 0, 0, 1)
    Attributes.Define(self.entity, "DamageFlash", 0, 0, 1)
    Attributes.Define(self.entity, "HitConfirm", 0, 0, 1)
    for _, enemy in ipairs(world:EntitiesWith("Tags")) do
        if enemy:Has("Tags") and enemy:Has("Transform") then
            for _, tag in ipairs(enemy:Get("Tags").names) do
                if tag == "Scout" then
                    local p = enemy:Get("Transform").position
                    self.scout_spawn[enemy:Guid()] = vector.create(p.x, p.y, p.z)
                    Attributes.Define(enemy, "Health", 3, 0, 3)
                end
            end
        end
    end
    if self.character ~= nil then
        self.character.walk_speed = self.speed
        self.character.jump_velocity = self.jump_speed
    end
end

local function Quat(yaw, pitch)
    local hy, hp = yaw / 2, pitch / 2
    return {
        math.cos(hy) * math.sin(hp), math.sin(hy) * math.cos(hp),
        -math.sin(hy) * math.sin(hp), math.cos(hy) * math.cos(hp),
    }
end

function Controller:OnUpdate(dt)
    local t = self.entity:Get("Transform")

    if self.dead then
        if Input.IsTriggered("Restart") then
            self.dead = false
            self.health = 3
            self.ammo = self.ammo_capacity
            self.reloading = false
            self.reload_timer = 0
            self.damage_flash = 0
            self.hit_confirm = 0
            self.damage_timer = 0
            Attributes.SetBase(self.entity, "Health", self.health)
            Attributes.SetBase(self.entity, "Ammo", self.ammo)
            Attributes.SetBase(self.entity, "Reloading", 0)
            Attributes.SetBase(self.entity, "DamageFlash", 0)
            Attributes.SetBase(self.entity, "HitConfirm", 0)
            self.scout_health = {}
            t.position = self.spawn_position
            self.vy = 0
            for _, enemy in ipairs(world:EntitiesWith("Tags")) do
                if enemy:IsValid() and enemy:Has("Tags") and enemy:Has("Transform") then
                    for _, tag in ipairs(enemy:Get("Tags").names) do
                        if tag == "Scout" then
                            local guid = enemy:Guid()
                            enemy:Get("Transform").position = self.scout_spawn[guid]
                            self.scout_health[guid] = nil
                            Attributes.SetBase(enemy, "Health", 3)
                        end
                    end
                end
            end
        else
            if self.character ~= nil then
                self.character.input = vector.create(0, 0, 0)
                self.character.jump_requested = false
            end
            return
        end
    end

    local look = Input.GetAxis2D("Look")
    self.yaw = self.yaw - math.rad(look.x * self.look_speed)
    self.pitch = math.clamp(self.pitch - math.rad(look.y * self.look_speed), -1.2, 0.5)

    -- Move relative to the camera's heading.
    local move = Input.GetAxis2D("Move")
    if vector.magnitude(move) > 1 then move = vector.normalize(move) end
    local sy, cy = math.sin(self.yaw), math.cos(self.yaw)
    local vx = -sy * move.y + cy * move.x
    local vz = -cy * move.y - sy * move.x

    if self.character ~= nil then
        -- The player's existing Jolt CharacterSystem consumes this input in
        -- FixedUpdate, including slopes, steps, collision and buffered jumps.
        self.character.input = vector.create(vx, 0, vz)
        self.character.jump_requested = Input.IsTriggered("Jump")
    else
        -- Preserve this template's movement when the engine is built without
        -- the optional physics module.
        if self.vy == 0 and Input.IsTriggered("Jump") then self.vy = self.jump_speed end
        self.vy += self.gravity * dt
        local p = t.position
        local y = p.y + self.vy * dt
        if y <= self.floor then y = self.floor; self.vy = 0 end
        t.position = vector.create(p.x + vx * self.speed * dt, y, p.z + vz * self.speed * dt)
    end

    -- A simple, game-owned hitscan loop for the first combat proof. Select the
    -- closest Scout inside a narrow cone in front of the over-shoulder camera.
    self.fire_timer = math.max(0, self.fire_timer - dt)
    self.damage_flash = math.max(0, self.damage_flash - dt * 2.5)
    self.hit_confirm = math.max(0, self.hit_confirm - dt * 3.0)
    Attributes.SetBase(self.entity, "DamageFlash", self.damage_flash)
    Attributes.SetBase(self.entity, "HitConfirm", self.hit_confirm)

    if Input.IsTriggered("Reload") and not self.reloading and self.ammo < self.ammo_capacity then
        self.reloading = true
        self.reload_timer = 1.1
        Attributes.SetBase(self.entity, "Reloading", 1)
    end
    if self.reloading then
        self.reload_timer = self.reload_timer - dt
        if self.reload_timer <= 0 then
            self.reloading = false
            self.ammo = self.ammo_capacity
            Attributes.SetBase(self.entity, "Ammo", self.ammo)
            Attributes.SetBase(self.entity, "Reloading", 0)
        end
    end

    if Input.IsTriggered("Fire") and self.fire_timer == 0 and not self.reloading and self.ammo > 0 then
        self.fire_timer = self.fire_cooldown
        self.ammo -= 1
        Attributes.SetBase(self.entity, "Ammo", self.ammo)
        local fx = -sy * math.cos(self.pitch)
        local fy = math.sin(self.pitch)
        local fz = -cy * math.cos(self.pitch)
        local origin = t.position + vector.create(0, self.height, 0)
        local target, best_distance = nil, self.weapon_range
        for _, enemy in ipairs(world:EntitiesWith("Tags")) do
            if enemy:IsValid() and enemy:Has("Tags") then
                local tags = enemy:Get("Tags").names
                local is_scout = false
                for _, tag in ipairs(tags) do
                    if tag == "Scout" then is_scout = true end
                end
                if is_scout and enemy:Has("Transform") then
                    local p = enemy:Get("Transform").position
                    local dx, dy, dz = p.x - origin.x, p.y - origin.y, p.z - origin.z
                    local distance = math.sqrt(dx * dx + dy * dy + dz * dz)
                    if distance > 0 and distance < best_distance then
                        local alignment = (dx * fx + dy * fy + dz * fz) / distance
                        if alignment > 0.94 and HasClearShot(origin, p, self.entity, enemy) then
                            target, best_distance = enemy, distance
                        end
                    end
                end
            end
        end
        if target ~= nil then
            self.hit_confirm = 1
            Attributes.SetBase(self.entity, "HitConfirm", self.hit_confirm)
            local guid = target:Guid()
            local health = (self.scout_health[guid] or 3) - 1
            if health <= 0 then
                self.scout_health[guid] = 0
                Attributes.SetBase(target, "Health", 0)
                local p = target:Get("Transform").position
                target:Get("Transform").position = vector.create(p.x, -100, p.z)
            else
                self.scout_health[guid] = health
                Attributes.SetBase(target, "Health", health)
                local hit_transform = target:Get("Transform")
                local p = hit_transform.position
                local dx, dz = p.x - origin.x, p.z - origin.z
                local length = math.sqrt(dx * dx + dz * dz)
                if length > 0 then
                    hit_transform.position = vector.create(p.x + dx / length * 0.3, p.y, p.z + dz / length * 0.3)
                end
            end
        end
    end

    self.damage_timer = math.max(0, self.damage_timer - dt)
    if self.damage_timer == 0 then
        for _, enemy in ipairs(world:EntitiesWith("Tags")) do
            if enemy:IsValid() and enemy:Has("Tags") and enemy:Has("Transform") then
                local is_scout = false
                for _, tag in ipairs(enemy:Get("Tags").names) do
                    if tag == "Scout" then is_scout = true end
                end
                local guid = enemy:Guid()
                if is_scout and self.scout_health[guid] ~= 0 then
                    local p = enemy:Get("Transform").position
                    local dx, dz = p.x - t.position.x, p.z - t.position.z
                    if dx * dx + dz * dz < 1.8 * 1.8 then
                        self.health -= 1
                        self.damage_flash = 1
                        Attributes.SetBase(self.entity, "Health", self.health)
                        Attributes.SetBase(self.entity, "DamageFlash", self.damage_flash)
                        self.damage_timer = 0.8
                        if self.health <= 0 then self.dead = true end
                        break
                    end
                end
            end
        end
    end

    -- Face the way it's going.
    if vx ~= 0 or vz ~= 0 then
        local heading = math.atan2(-vx, -vz)
        t.rotation = {0, math.sin(heading / 2), 0, math.cos(heading / 2)}
    end

    -- The camera sits behind and above, looking at the character.
    if self.camera ~= nil and self.camera:IsValid() then
        local forward = vector.create(-sy * math.cos(self.pitch), math.sin(self.pitch), -cy * math.cos(self.pitch))
        local target = t.position + vector.create(0, self.height, 0)
        local ct = self.camera:Get("Transform")
        ct.position = target - forward * self.distance
        ct.rotation = Quat(self.yaw, self.pitch)
    end
end

return Controller
