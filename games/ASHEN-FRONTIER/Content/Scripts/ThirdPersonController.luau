-- Third person controller: put it on the character (tagged Player). The Look
-- axis orbits the camera (the entity with a Camera component) around it, WASD or
-- the left stick moves relative to where the camera looks, and the character
-- turns to face where it goes. Space or the A button jumps.
local Controller = {}

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
--@tooltip The camera stops short of a wall or a hill between it and the character (needs physics)
Controller.camera_collision = true
--@range 0.05 1
--@tooltip How big the camera is for that: a sphere of this radius, metres
Controller.camera_radius = 0.3
--@range 0 20
Controller.jump_speed = 5.5
--@range -60 0
Controller.gravity = -15

function Controller:OnCreate()
    self.yaw = 0
    self.pitch = -0.3
    self.vy = 0
    self.floor = self.entity:Get("Transform").position.y
    self.camera = world:EntitiesWith("Camera")[1]
    self.camera_now = self.distance -- how far behind it is now (it comes in at once and goes back out gently)
    self.character = self.entity:Get("CharacterMovement")
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
        -- Something between the character's head and where the camera wants to be (a hill, a wall): a sphere swept back from the head stops short of it.
        local clear = self.distance
        if self.camera_collision then
            local back = vector.create(-forward.x, -forward.y, -forward.z)
            local ok, hit, _, _, _, hit_distance = pcall(function() return Physics.SphereCast(target, back, self.camera_radius, self.distance, self.entity) end)
            if ok and hit then clear = math.max(hit_distance, 0.3) end
        end
        if clear < self.camera_now then self.camera_now = clear
        else self.camera_now = self.camera_now + (clear - self.camera_now) * math.min(1, 6 * dt) end
        ct.position = target - forward * self.camera_now
        ct.rotation = Quat(self.yaw, self.pitch)
    end
end

return Controller
