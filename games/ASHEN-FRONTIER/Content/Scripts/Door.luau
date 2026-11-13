-- A door: walk into it and the game moves to the scene named by `target` (the Scene table). Put it on an entity placed in the doorway.
local Door = {}

--@tooltip The content path of the scene this door leads to, such as Scenes/AssemblyHall.ascene
Door.target = ""
--@tooltip The name of an entity in that scene where the player appears (empty: the scene's own start)
Door.spawn = ""
--@range 0.5 10
--@tooltip How close the player has to come, metres (along the ground)
Door.radius = 2.2

local function Sfx(name)
    if not Audio.PlaySound2D("Audio/" .. name .. ".acue") then print("Missing sound: " .. name) end
end

function Door:OnCreate()
    self.player = world:EntitiesWith("CharacterMovement")[1]
end

function Door:OnUpdate(dt)
    if self.used or self.player == nil or self.target == "" then return end
    local a = self.entity:Get("Transform").position
    local b = self.player:Get("Transform").position
    local dx, dz = a.x - b.x, a.z - b.z
    if dx * dx + dz * dz < self.radius * self.radius and math.abs(a.y - b.y) < 3 then
        Sfx("door")
        self.used = true -- one request; the game swaps scenes at the start of the next frame
        Scene.Load(self.target, self.spawn ~= "" and self.spawn or nil)
    end
end

return Door
