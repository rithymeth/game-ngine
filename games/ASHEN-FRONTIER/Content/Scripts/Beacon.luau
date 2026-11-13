-- The distress relay: walk up to it and it is reached (saved as "mission.relay"), which finishes the first chapter's last objective.
local Beacon = {}

--@range 1 30
--@tooltip How close Kael has to come, metres (along the ground)
Beacon.radius = 8

function Beacon:OnCreate()
    self.player = world:EntitiesWith("CharacterMovement")[1]
end

function Beacon:OnUpdate(dt)
    if self.player == nil or SaveGames.GetNumber("mission.relay", 0) > 0 then return end
    local a = self.entity:Get("Transform").position
    local b = self.player:Get("Transform").position
    local dx, dz = a.x - b.x, a.z - b.z
    if dx * dx + dz * dz <= self.radius * self.radius then
        SaveGames.SetNumber("mission.relay", 1)
        SaveGames.SaveToSlot("autosave")
        Camera.Shake(0.35)
        print("Distress relay reached")
    end
end

return Beacon
