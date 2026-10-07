-- First Contact Scout: a deliberately simple greybox pressure enemy.
local Scout = {}

--@range 0 10
Scout.speed = 1.2
--@range 0 20
Scout.stop_distance = 3

function Scout:OnUpdate(dt)
    local players = world:EntitiesWith("CharacterMovement")
    if #players == 0 then return end
    local transform = self.entity:Get("Transform")
    local player = players[1]:Get("Transform").position
    local p = transform.position
    local dx, dz = player.x - p.x, player.z - p.z
    local distance = math.sqrt(dx * dx + dz * dz)
    if distance > self.stop_distance then
        local step = math.min(self.speed * dt, distance - self.stop_distance)
        transform.position = vector.create(p.x + dx / distance * step, p.y, p.z + dz / distance * step)
    end
end

return Scout
