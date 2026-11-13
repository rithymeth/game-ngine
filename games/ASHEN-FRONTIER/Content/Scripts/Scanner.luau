-- Kael's scanner (the Scan action: Q, or Y on a pad). A pulse lists what is near by category, as words rather than colours: [SALVAGE], [MACHINE],
-- [ROBOT], [FABRICATION], [POWER], [RELAY], [HAZARD], each with its distance and a compass direction (x east, z south), nearest first, and any status
-- that is known (a sentinel's health, whether the generator runs). The list stays on the HUD for a few seconds. Put it on the HUD entity.
local Scanner = {}

--@range 5 100
--@tooltip How far the pulse reaches, metres
Scanner.range = 35
--@range 1 15
--@tooltip How long the list stays on screen, seconds
Scanner.show_for = 7
--@range 1 5
Scanner.lines = 5

local function Sfx(name)
    if not Audio.PlaySound2D("Audio/" .. name .. ".acue") then print("Missing sound: " .. name) end
end

-- tag -> the category word the scan shows (the first tag of an entity found in this list decides)
local CATEGORIES = {
    { "Enemy", "MACHINE" }, { "MULE3", "ROBOT" }, { "Workbench", "FABRICATION" }, { "Generator", "POWER" },
    { "Relay", "RELAY" }, { "Hazard", "HAZARD" }, { "Console", "CONSOLE" }, { "Terminal", "TERMINAL" }, { "Sample", "SAMPLE" }, { "Salvage", "SALVAGE" },
}

local function Compass(dx, dz)
    local angle = math.deg(math.atan2(dx, -dz)) -- 0 = north (-z), 90 = east (+x)
    local names = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" }
    return names[(math.floor((angle + 22.5) / 45) % 8) + 1]
end

local function Status(category, name)
    if category == "MACHINE" then
        local hp = SaveGames.GetNumber("enemy." .. name .. ".hp", -1)
        if hp >= 0 then return string.format("health %d", hp) end
    elseif category == "ROBOT" then
        return SaveGames.GetNumber("mule.repaired", 0) > 0 and "repaired" or "broken: needs repair"
    elseif category == "POWER" then
        return SaveGames.GetNumber("power.outpost", 0) > 0 and "running" or "off"
    elseif category == "CONSOLE" then
        return SaveGames.GetNumber("mission.dome", 0) > 0 and "running" or "needs 3 seed samples"
    elseif category == "TERMINAL" then
        return SaveGames.GetNumber("mission.assembly", 0) > 0 and "running" or "locked while the wardens are active"
    elseif category == "RELAY" then
        return SaveGames.GetNumber("mission.relay", 0) > 0 and "reached" or "not reached"
    end
    return nil
end

function Scanner:OnCreate()
    self.player = world:EntitiesWith("CharacterMovement")[1]
    self.cool = 0
    self.hide_in = 0
end

function Scanner:OnUpdate(dt)
    if self.player == nil then return end
    self.cool = self.cool - dt
    if self.hide_in > 0 then
        self.hide_in = self.hide_in - dt
        if self.hide_in <= 0 then UI.SetVisible("ScanPanel", false) end
    end
    if self.cool > 0 or not Input.IsTriggered("Scan") then return end
    self.cool = 1
    Sfx("scan")
    local here = self.player:Get("Transform").position
    local found = {}
    for _, e in ipairs(world:EntitiesWith("Tags")) do
        local tags = e:Get("Tags").names
        local category = nil
        for _, entry in ipairs(CATEGORIES) do
            for _, t in ipairs(tags) do if t == entry[1] and category == nil then category = entry[2] end end
            if category ~= nil then break end
        end
        if category ~= nil then
            local p = e:Get("Transform").position
            local dx, dz = p.x - here.x, p.z - here.z
            local d = math.sqrt(dx * dx + dz * dz)
            if d <= self.range then
                local name = e:Get("EntityName").value
                found[#found + 1] = { d = d, category = category, name = name, dir = Compass(dx, dz) }
            end
        end
    end
    table.sort(found, function(a, b) return a.d < b.d end)
    for i = 1, self.lines do
        local f = found[i]
        local text = ""
        if f ~= nil then
            local pretty = string.gsub(string.gsub(f.name, "_%d+", ""), "_", " ")
            local status = Status(f.category, f.name)
            text = string.format("[%s] %s  %d m %s%s", f.category, pretty, math.floor(f.d + 0.5), f.dir, status and ("  (" .. status .. ")") or "")
            print("Scan " .. text)
        end
        UI.SetText("ScanLine" .. i, text)
    end
    if #found == 0 then
        UI.SetText("ScanLine1", "Nothing in range.")
        print("Scan nothing in range")
    end
    UI.SetVisible("ScanPanel", true)
    self.hide_in = self.show_for
end

return Scanner
