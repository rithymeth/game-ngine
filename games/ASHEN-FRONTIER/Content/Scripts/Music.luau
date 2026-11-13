-- The music: a calm loop, and a tense one while a sentinel that is still active is within `danger_range` of Kael. The loop is started with Audio.Play2D
-- (which gives a handle) and, when the mood changes, the old one is faded out with Audio.Stop and the new one started. Put it on an entity of its own; a scene
-- change ends the audio system with the scene, so each scene starts its own loop and they never stack.
local Music = {}

--@tooltip The calm loop (a sound cue in the package)
Music.calm = "Audio/ambient_calm.acue"
--@tooltip The loop while a sentinel is close
Music.danger = "Audio/ambient_danger.acue"
--@range 1 60
--@tooltip How close a sentinel has to be, metres
Music.danger_range = 16
--@range 0 5
--@tooltip Seconds the old loop takes to fade out
Music.fade = 1.5
--@range -40 0
Music.volume_db = -10

function Music:OnCreate()
    self.player = world:EntitiesWith("CharacterMovement")[1]
    self.mode = nil
    self.handle = false
    self.check_in = 0
end

local function Danger(self)
    if self.player == nil then return false end
    local here = self.player:Get("Transform").position
    local r2 = self.danger_range * self.danger_range
    for _, e in ipairs(world:EntitiesWith("Tags")) do
        for _, t in ipairs(e:Get("Tags").names) do
            if t == "Enemy" then
                local name = e:Get("EntityName").value
                if SaveGames.GetNumber("enemy." .. name .. ".hp", 1) > 0 then
                    local p = e:Get("Transform").position
                    local dx, dz = p.x - here.x, p.z - here.z
                    if dx * dx + dz * dz <= r2 then return true end
                end
            end
        end
    end
    return false
end

function Music:OnUpdate(dt)
    self.check_in = self.check_in - dt
    if self.check_in > 0 then return end
    self.check_in = 0.4
    local want = Danger(self) and "danger" or "calm"
    local ended = self.handle and not Audio.IsPlaying(self.handle) -- a loop that has stopped by itself is started again
    if want ~= self.mode or ended then
        if self.handle then Audio.Stop(self.handle, self.fade) end
        self.handle = Audio.Play2D(want == "danger" and self.danger or self.calm, self.volume_db)
        if not self.handle then print("Missing music: " .. want) end
        self.mode = want
        print("Music: " .. want)
    end
end

return Music
