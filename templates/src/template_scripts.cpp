#include "template_scripts.h"

namespace aether::templates {

const char* const kFirstPersonScript = R"LUA(-- First person controller: WASD or the left stick to move, the mouse or the
-- right stick to look, Space or the A button to jump. Put it on the entity that
-- has the Camera; the height it starts at is the floor.
local Controller = {}

--@range 0 30
--@tooltip Walking speed, metres per second
Controller.speed = 5
--@range 0 1
--@tooltip Degrees of turn per pixel of mouse movement
Controller.look_speed = 0.12
--@range 0 20
Controller.jump_speed = 5
--@range -60 0
Controller.gravity = -15

function Controller:OnCreate()
    self.yaw = 0     -- radians, turning left is positive
    self.pitch = 0   -- radians, looking up is positive
    self.vy = 0
    self.floor = self.entity:Get("Transform").position.y
end

function Controller:OnUpdate(dt)
    local t = self.entity:Get("Transform")

    local look = Input.GetAxis2D("Look")
    self.yaw = self.yaw - math.rad(look.x * self.look_speed)
    self.pitch = math.clamp(self.pitch - math.rad(look.y * self.look_speed), -1.4, 1.4)

    local move = Input.GetAxis2D("Move")
    if vector.magnitude(move) > 1 then move = vector.normalize(move) end
    local sy, cy = math.sin(self.yaw), math.cos(self.yaw)
    -- Forward is (-sin, 0, -cos) and right is (cos, 0, -sin): the camera looks down -Z.
    local vx = (-sy * move.y + cy * move.x) * self.speed
    local vz = (-cy * move.y - sy * move.x) * self.speed

    if self.vy == 0 and Input.IsTriggered("Jump") then self.vy = self.jump_speed end
    self.vy += self.gravity * dt
    local p = t.position
    local y = p.y + self.vy * dt
    if y <= self.floor then y = self.floor; self.vy = 0 end

    t.position = vector.create(p.x + vx * dt, y, p.z + vz * dt)
    -- Yaw about Y, then pitch about X, as a quaternion (x, y, z, w).
    local hy, hp = self.yaw / 2, self.pitch / 2
    t.rotation = {
        math.cos(hy) * math.sin(hp), math.sin(hy) * math.cos(hp),
        -math.sin(hy) * math.sin(hp), math.cos(hy) * math.cos(hp),
    }
end

return Controller
)LUA";

const char* const kThirdPersonScript = R"LUA(-- Third person controller: put it on the character (tagged Player). The Look
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
    @@CHARACTER_MOVEMENT_LOOKUP@@
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
)LUA";

const char* const kTopDownScript = R"LUA(-- Top down controller: put it on the character (tagged Player). WASD or the
-- left stick moves along the world's axes (up the screen is -Z), the character
-- turns to face where it goes, and the camera (the entity with a Camera
-- component) follows from above, tilted.
local Controller = {}

--@range 0 30
Controller.speed = 6
--@range 5 60
--@tooltip How far above the character the camera sits
Controller.camera_distance = 18
--@range 10 90
--@tooltip Camera angle below the horizon, degrees (90 looks straight down)
Controller.camera_tilt = 65
--@range 0 1
--@tooltip How quickly the camera catches up (1 is at once)
Controller.camera_follow = 0.15

function Controller:OnCreate()
    self.camera = world:EntitiesWith("Camera")[1]
end

function Controller:OnUpdate(dt)
    local t = self.entity:Get("Transform")
    local move = Input.GetAxis2D("Move")
    if vector.magnitude(move) > 1 then move = vector.normalize(move) end
    local vx, vz = move.x * self.speed, -move.y * self.speed
    local p = t.position
    t.position = vector.create(p.x + vx * dt, p.y, p.z + vz * dt)
    if vx ~= 0 or vz ~= 0 then
        local heading = math.atan2(-vx, -vz)
        t.rotation = {0, math.sin(heading / 2), 0, math.cos(heading / 2)}
    end

    if self.camera ~= nil and self.camera:IsValid() then
        local tilt = math.rad(self.camera_tilt)
        -- Behind the character (+Z) and above, looking down at it.
        local wanted = t.position + vector.create(0, math.sin(tilt), math.cos(tilt)) * self.camera_distance
        local ct = self.camera:Get("Transform")
        ct.position = ct.position + (wanted - ct.position) * math.min(1, self.camera_follow * dt * 60)
        local half = -tilt / 2
        ct.rotation = {math.sin(half), 0, 0, math.cos(half)}
    end
end

return Controller
)LUA";

const char* const kVehicleScript = R"LUA(-- Arcade vehicle controller: put it on the vehicle (tagged Player). W or the
-- left stick forward is the throttle, S brakes and reverses, A and D steer. A
-- chase camera (the entity with a Camera component) follows behind it.
local Vehicle = {}

--@range 0 80
Vehicle.max_speed = 28
--@range 0 20
Vehicle.reverse_speed = 8
--@range 0 60
--@tooltip Metres per second gained each second at full throttle
Vehicle.acceleration = 14
--@range 0 60
Vehicle.braking = 30
--@range 0 20
--@tooltip Speed lost each second with no throttle
Vehicle.drag = 5
--@range 0 180
--@tooltip Degrees per second of turn at full lock, once rolling
Vehicle.turn_rate = 100
--@range 2 30
Vehicle.camera_distance = 9
--@range 0 10
Vehicle.camera_height = 3.5

function Vehicle:OnCreate()
    self.heading = 0 -- radians, turning left is positive
    self.speed = 0   -- along the heading, negative in reverse
    self.camera = world:EntitiesWith("Camera")[1]
end

function Vehicle:OnUpdate(dt)
    local t = self.entity:Get("Transform")
    local move = Input.GetAxis2D("Move")
    local throttle, steer = move.y, move.x

    if throttle > 0 then
        -- Braking when the throttle opposes the motion, accelerating otherwise.
        local rate = self.speed < 0 and self.braking or self.acceleration
        self.speed = math.min(self.max_speed, self.speed + throttle * rate * dt)
    elseif throttle < 0 then
        local rate = self.speed > 0 and self.braking or self.acceleration
        self.speed = math.max(-self.reverse_speed, self.speed + throttle * rate * dt)
    else
        local lost = self.drag * dt
        if math.abs(self.speed) <= lost then self.speed = 0 else self.speed -= math.sign(self.speed) * lost end
    end

    -- Steering needs speed: none at a standstill, full from about 5 m/s, and
    -- reversed going backwards, like a real car.
    local grip = math.clamp(self.speed / 5, -1, 1)
    self.heading = self.heading - math.rad(steer * self.turn_rate) * grip * dt

    local sin, cos = math.sin(self.heading), math.cos(self.heading)
    local forward = vector.create(-sin, 0, -cos)
    t.position = t.position + forward * self.speed * dt
    t.rotation = {0, math.sin(self.heading / 2), 0, math.cos(self.heading / 2)}

    if self.camera ~= nil and self.camera:IsValid() then
        local ct = self.camera:Get("Transform")
        ct.position = t.position - forward * self.camera_distance + vector.create(0, self.camera_height, 0)
        local pitch = -math.atan2(self.camera_height, self.camera_distance)
        local hy, hp = self.heading / 2, pitch / 2
        ct.rotation = {
            math.cos(hy) * math.sin(hp), math.sin(hy) * math.cos(hp),
            -math.sin(hy) * math.sin(hp), math.cos(hy) * math.cos(hp),
        }
    end
end

return Vehicle
)LUA";

// A limb that swings with the pace of the character tagged Player (the one with a CharacterMovement): put it on a limb's entity, which pivots at its own origin (a hip, a shoulder).
const char* const kLimbSwingScript = R"LUA(-- Swings its entity about its own X axis in step with how fast the Player is going: still when it stands, a walk cycle when it moves. Put it on an arm or a leg that is a child of the
-- character, its origin at the joint. A leg and the arm on the other side take phases half a cycle apart.
local Limb = {}

--@range 0 90
--@tooltip How far the limb swings each way at a run, degrees
Limb.swing = 35
--@range -6.3 6.3
--@tooltip Where in the cycle this limb is, radians (a leg and its opposite arm share one, the other pair is 3.14 later)
Limb.phase = 0
--@range 0 2
--@tooltip Cycles of the walk per metre travelled
Limb.stride = 0.5
--@range 0 10
--@tooltip The speed (metres per second) at which the swing is complete
Limb.full_speed = 3

function Limb:OnCreate()
    self.player = world:EntitiesWith("CharacterMovement")[1]
    self.last = nil
    self.cycle = 0
    self.amount = 0
end

function Limb:OnUpdate(dt)
    local speed = 0
    if self.player ~= nil and self.player:IsValid() then
        local p = self.player:Get("Transform").position
        if self.last ~= nil and dt > 0.0001 then
            local dx, dz = p.x - self.last.x, p.z - self.last.z
            speed = math.sqrt(dx * dx + dz * dz) / dt
        end
        self.last = p
    end
    self.cycle = self.cycle + speed * dt * self.stride
    -- Ease the swing in and out, so a stop settles and a start does not jerk.
    local wanted = math.min(1, speed / self.full_speed)
    self.amount = self.amount + (wanted - self.amount) * math.min(1, 10 * dt)
    local angle = math.rad(self.swing) * self.amount * math.sin(self.cycle * 6.2831853 + self.phase)
    self.entity:Get("Transform").rotation = {math.sin(angle / 2), 0, 0, math.cos(angle / 2)}
end

return Limb
)LUA";

} // namespace aether::templates
