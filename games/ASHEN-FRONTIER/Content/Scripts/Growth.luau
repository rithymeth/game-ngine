-- A plant that grows when `flag` is set in SaveGames (mission.dome...): small until then, growing to the size the scene gave it. The flag is read every
-- frame, like FlagLight does.
local Growth = {}

--@tooltip The SaveGames key that makes it grow
Growth.flag = "mission.dome"
--@range 0.05 1
--@tooltip Its size, as a part of full, until the flag is set
Growth.start = 0.35
--@range 0.05 5
--@tooltip How much of the full size it grows per second
Growth.rate = 0.4

function Growth:OnCreate()
    local t = self.entity:Get("Transform")
    self.full = t.scale
    self.factor = self.start
    self.reported = false
    t.scale = self.full * self.factor
end

function Growth:OnUpdate(dt)
    local target = SaveGames.GetNumber(self.flag, 0) > 0 and 1 or self.start
    self.factor = self.factor + math.clamp(target - self.factor, -self.rate * dt, self.rate * dt)
    self.entity:Get("Transform").scale = self.full * self.factor
    if target == 1 and self.factor == 1 and not self.reported then
        self.reported = true
        print("Plant grown: " .. self.entity:Get("EntityName").value)
    end
end

return Growth
