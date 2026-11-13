-- A sentinel: it stands guard until Kael comes within `detect_range`, then walks at him and strikes when it is close. Kael hits back (the Attack action,
-- handled by his Interactor), and a sentinel at no health is disabled: it leaves the world for good and drops components. Health lives in SaveGames
-- under "enemy.<name>.hp", so a sentinel that was disabled stays disabled through doors and restarts, and Kael's health is "player.hp".
local Sentinel = {}

--@range 1 1000
Sentinel.max_hp = 100
--@range 1 60
--@tooltip How close Kael has to be for it to notice him, metres
Sentinel.detect_range = 14
--@range 0 12
Sentinel.speed = 3.2
--@range 0.5 6
Sentinel.attack_range = 1.8
--@range 0 100
Sentinel.damage = 10
--@range 0.2 10
--@tooltip Seconds between strikes
Sentinel.cooldown = 1.2
--@tooltip Components it drops when disabled
Sentinel.reward = 2

local function Quat(yaw)
    local h = yaw / 2
    return { 0, math.sin(h), 0, math.cos(h) }
end

local function Sfx(name)
    if not Audio.PlaySound2D("Audio/" .. name .. ".acue") then print("Missing sound: " .. name) end
end

function Sentinel:OnCreate()
    self.player = world:EntitiesWith("CharacterMovement")[1]
    self.key = "enemy." .. self.entity:Get("EntityName").value .. ".hp"
    self.cool = 0.8 -- a moment before the first strike
end

function Sentinel:OnUpdate(dt)
    -- asked here, not in OnCreate: the script that loads the autosave (Kael's Interactor) may be created after this one
    if not self.ready then
        self.ready = true
        if SaveGames.HasKey(self.key) and SaveGames.GetNumber(self.key, 0) <= 0 then world:Destroy(self.entity) return end -- disabled in an earlier visit
        if not SaveGames.HasKey(self.key) then SaveGames.SetNumber(self.key, self.max_hp) end
    end
    local hp = SaveGames.GetNumber(self.key, self.max_hp)
    if hp <= 0 then
        local components = SaveGames.GetNumber("pack.components", 0) + self.reward
        SaveGames.SetNumber("pack.components", components)
        SaveGames.SaveToSlot("autosave")
        UI.SetText("ComponentText", string.format("COMPONENTS  %d", components))
        Camera.Shake(0.2)
        Sfx("sentinel_down")
        print(string.format("Sentinel disabled: +%d components", self.reward))
        world:Destroy(self.entity)
        return
    end
    if self.player == nil then return end
    local t = self.entity:Get("Transform")
    local p = self.player:Get("Transform").position
    local dx, dz = p.x - t.position.x, p.z - t.position.z
    local dist = math.sqrt(dx * dx + dz * dz)
    if dist < self.detect_range then
        t.rotation = Quat(math.atan2(dx, dz))
        if dist > self.attack_range then
            local step = math.min(self.speed * dt, dist - self.attack_range)
            t.position = vector.create(t.position.x + dx / dist * step, p.y, t.position.z + dz / dist * step)
        end
    end
    self.cool = self.cool - dt
    if dist <= self.attack_range and self.cool <= 0 then
        self.cool = self.cooldown
        local left = SaveGames.GetNumber("player.hp", 100) - self.damage
        SaveGames.SetNumber("player.hp", left)
        Camera.Shake(0.3)
        Sfx("hurt")
        print(string.format("Sentinel hit Kael (health %d)", left))
    end
end

return Sentinel
