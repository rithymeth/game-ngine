-- Compatibility corpus (M8.1): an entity script class as the samples write it. The test attaches it to an
-- entity, runs BeginPlay, two 0.5 s updates and an OnHit event, then calls CompatCheck.
local Mover = {}
Mover.speed = 2
function Mover:OnStart()
    self.started = true
    self.entity:Add('Transform')
    self.timer_hits = 0
    Timer.Every(0.5, function() self.timer_hits += 1 end) -- owned by this entity
end
function Mover:OnUpdate(dt)
    self.t = (self.t or 0) + dt * self.speed
    local tr = self.entity:Get('Transform')
    tr.position = vector.create(self.t, 0, 0)
end
function Mover:OnHit(amount) self.hit = amount end
function Mover:CompatCheck()
    assert(self.started, 'OnStart did not run')
    assert(self.t == 2, 'OnUpdate total ' .. tostring(self.t))
    assert(self.entity:Get('Transform').position.x == 2)
    assert(self.hit == 3, 'OnHit argument')
    assert(self.timer_hits == 2, 'timer hits ' .. self.timer_hits)
    self.compat_ok = true
end
return Mover
