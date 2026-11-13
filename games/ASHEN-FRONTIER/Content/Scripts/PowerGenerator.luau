-- The outpost's generator. It needs the Generator Core from the fab bench; Interact beside it then powers it (saved as "power.outpost"), and its light
-- comes up. Put it on the generator's entity, which carries a PointLight (dark until it is powered).
local Generator = {}

--@range 0.5 8
Generator.reach = 3
--@tooltip How bright its light is when powered
Generator.glow = 14
--@range 0.1 10
--@tooltip How quickly the light comes up, in units per second
Generator.ramp = 10

function Generator:OnCreate()
    self.player = world:EntitiesWith("CharacterMovement")[1]
    self.prompt = nil
    self.light = self.entity:Get("PointLight")
    self.level = 0
    if self.light ~= nil then self.light.intensity = 0 end
end

local function Prompt(self, text)
    if text == self.prompt then return end
    if UI.SetVisible("PromptPanel", text ~= nil) then
        self.prompt = text
        if text ~= nil then UI.SetText("PromptText", text) end
    end
end

function Generator:OnUpdate(dt)
    local powered = SaveGames.GetNumber("power.outpost", 0) > 0
    if self.light ~= nil then
        local target = powered and self.glow or 0
        self.level = self.level + math.clamp(target - self.level, -self.ramp * dt, self.ramp * dt)
        self.light.intensity = self.level
    end
    if self.player == nil then return end
    local a = self.entity:Get("Transform").position
    local b = self.player:Get("Transform").position
    local dx, dz = a.x - b.x, a.z - b.z
    if dx * dx + dz * dz > self.reach * self.reach then
        if self.prompt ~= nil then Prompt(self, nil) end
        return
    end
    if powered then
        Prompt(self, "The outpost generator is running")
    elseif SaveGames.GetNumber("crafted.generator_core", 0) > 0 then
        Prompt(self, "[E] Install the Generator Core and start the generator")
        if Input.IsTriggered("Interact") then
            SaveGames.SetNumber("power.outpost", 1)
            SaveGames.SaveToSlot("autosave")
            Camera.Shake(0.4)
            self.prompt = nil
            print("Generator powered")
        end
    else
        Prompt(self, "The generator needs a Generator Core (craft one at the fab bench)")
    end
end

return Generator
