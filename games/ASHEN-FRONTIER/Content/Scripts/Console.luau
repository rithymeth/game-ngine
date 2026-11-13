-- The living dome's irrigation console. Three seed samples (collected in the Verdant Array) wake it: Interact beside it spends them, the dome's garden is
-- restored ("mission.dome", saved) and the chapter is done. Put it on the console's entity.
local Console = {}

--@range 0.5 8
Console.reach = 3
--@tooltip Seed samples it takes
Console.samples_needed = 3

local function Sfx(name)
    if not Audio.PlaySound2D("Audio/" .. name .. ".acue") then print("Missing sound: " .. name) end
end

function Console:OnCreate()
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

function Console:OnUpdate(dt)
    if self.player == nil then return end
    local a = self.entity:Get("Transform").position
    local b = self.player:Get("Transform").position
    local dx, dz = a.x - b.x, a.z - b.z
    if dx * dx + dz * dz > self.reach * self.reach then
        if self.prompt ~= nil then Prompt(self, nil) end
        return
    end
    local have = SaveGames.GetNumber("pack.samples", 0)
    if SaveGames.GetNumber("mission.dome", 0) > 0 then
        Prompt(self, "The irrigation console is running")
    elseif have >= self.samples_needed then
        Prompt(self, string.format("[E] Load the %d seed samples into the console", self.samples_needed))
        if Input.IsTriggered("Interact") then
            SaveGames.SetNumber("pack.samples", have - self.samples_needed)
            SaveGames.SetNumber("mission.dome", 1)
            SaveGames.SaveToSlot("autosave")
            UI.SetText("SamplesText", string.format("SEED SAMPLES  %d / 3", have - self.samples_needed))
            Camera.Shake(0.4)
            Sfx("power_up")
            self.prompt = nil
            print("Living dome restored")
        end
    else
        Prompt(self, string.format("The console needs %d seed samples (you have %d)", self.samples_needed, have))
    end
end

return Console
