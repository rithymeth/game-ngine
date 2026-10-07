-- First Contact Scout: a deliberately simple greybox pressure enemy.
local Scout = {}

--@range 0 10
Scout.speed = 1.2
--@range 0 20
Scout.stop_distance = 1.2

function Scout:OnCreate()
    self.player = nil
    for _, entity in ipairs(world:EntitiesWith("Tags")) do
        for _, tag in ipairs(entity:Get("Tags").names) do
            if tag == "Player" then self.player = entity end
        end
    end
end

function Scout:OnUpdate(dt)
    if self.player == nil or not self.player:IsValid() then return end
    -- The cryo bay remains quiet until Kael retrieves the rifle. Defeated
    -- Scouts stay at their hidden position across checkpoint reloads.
    if Attributes.Get(self.player, "MissionStage") < 1 or Attributes.Get(self.entity, "Health") <= 0 then return end
    local transform = self.entity:Get("Transform")
    local player = self.player:Get("Transform").position
    local p = transform.position
    local dx, dz = player.x - p.x, player.z - p.z
    local distance = math.sqrt(dx * dx + dz * dz)
    if distance > self.stop_distance then
        local step = math.min(self.speed * dt, distance - self.stop_distance)
        transform.position = vector.create(p.x + dx / distance * step, p.y, p.z + dz / distance * step)
    end
end

return Scout
