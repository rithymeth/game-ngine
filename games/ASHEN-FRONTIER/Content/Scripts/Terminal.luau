-- The assembly hall's terminal. It stays locked while any of the sentinels named in `required` is still active (their health lives in SaveGames as
-- "enemy.<name>.hp"; a sentinel at no health, or one that was disabled and removed, no longer counts). Interact beside it once they are all down starts the
-- assembly line ("mission.assembly", saved): chapter 3's last step. Put it on the terminal's entity.
local Terminal = {}

--@range 0.5 8
Terminal.reach = 3
--@tooltip The sentinels (entity names, comma separated) that have to be disabled first
Terminal.required = "WardenSentinel_0,WardenSentinel_1"

local function Sfx(name)
    if not Audio.PlaySound2D("Audio/" .. name .. ".acue") then print("Missing sound: " .. name) end
end

function Terminal:OnCreate()
    self.player = world:EntitiesWith("CharacterMovement")[1]
    self.prompt = nil
end

local function Prompt(self, text)
    if text == self.prompt then return end
    if UI.SetVisible("PromptPanel", text ~= nil) then
        self.prompt = text
        if text ~= nil then UI.SetText("PromptText", text) end
    end
end

local function Remaining(self)
    local left = 0
    for name in string.gmatch(self.required, "[^,]+") do
        -- never met (no saved health) counts as still active
        if SaveGames.GetNumber("enemy." .. name .. ".hp", 1) > 0 then left = left + 1 end
    end
    return left
end

function Terminal:OnUpdate(dt)
    if self.player == nil then return end
    local a = self.entity:Get("Transform").position
    local b = self.player:Get("Transform").position
    local dx, dz = a.x - b.x, a.z - b.z
    if dx * dx + dz * dz > self.reach * self.reach then
        if self.prompt ~= nil then Prompt(self, nil) end
        return
    end
    local left = Remaining(self)
    if SaveGames.GetNumber("mission.assembly", 0) > 0 then
        Prompt(self, "The assembly line is running")
    elseif left > 0 then
        Prompt(self, string.format("Locked: %d warden sentinel(s) still guard the line", left))
        if Input.IsTriggered("Interact") then print("Terminal locked") end
    else
        Prompt(self, "[E] Start the assembly line")
        if Input.IsTriggered("Interact") then
            SaveGames.SetNumber("mission.assembly", 1)
            SaveGames.SaveToSlot("autosave")
            Camera.Shake(0.4)
            Sfx("power_up")
            self.prompt = nil
            print("Assembly line started")
        end
    end
end

return Terminal
