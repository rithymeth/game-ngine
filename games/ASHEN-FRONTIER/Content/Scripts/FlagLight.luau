-- A light that follows a saved flag: dim until `flag` is set in SaveGames (power.outpost, mission.dome...), then it fades up to full. The flag is read every
-- frame (not once in OnCreate: the script that loads the autosave may be created after this one), so entering a scene that was already restored fades the
-- light up in its first second. Put it on an entity with a PointLight.
local FlagLight = {}

--@tooltip The SaveGames key that switches it on
FlagLight.flag = "power.outpost"
--@range 0 100
--@tooltip Intensity while the flag is not set
FlagLight.off = 0
--@range 0 100
--@tooltip Intensity once it is set
FlagLight.on = 8
--@range 0.1 100
--@tooltip Intensity change per second
FlagLight.ramp = 8

function FlagLight:OnCreate()
    self.light = self.entity:Get("PointLight")
    self.level = self.off
    self.reported = false
    if self.light ~= nil then self.light.intensity = self.level end
end

function FlagLight:OnUpdate(dt)
    if self.light == nil then return end
    local target = SaveGames.GetNumber(self.flag, 0) > 0 and self.on or self.off
    self.level = self.level + math.clamp(target - self.level, -self.ramp * dt, self.ramp * dt)
    self.light.intensity = self.level
    if target == self.on and self.level == self.on and not self.reported then
        self.reported = true
        print("Light on: " .. self.entity:Get("EntityName").value)
    end
end

return FlagLight
