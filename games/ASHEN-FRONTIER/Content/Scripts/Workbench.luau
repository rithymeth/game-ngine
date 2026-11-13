-- The fab bench: where salvage becomes a part. Walk up to it and Interact to craft a Generator Core (the cost is spent from the pack in SaveGames,
-- and "crafted.generator_core" is set, which the generator and the objective line read).
local Workbench = {}

--@range 0.5 8
Workbench.reach = 3
--@tooltip Scrap the Generator Core costs
Workbench.scrap_cost = 4
--@tooltip Components the Generator Core costs
Workbench.component_cost = 2

local function Sfx(name)
    if not Audio.PlaySound2D("Audio/" .. name .. ".acue") then print("Missing sound: " .. name) end
end

function Workbench:OnCreate()
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

function Workbench:OnUpdate(dt)
    if self.player == nil then return end
    local a = self.entity:Get("Transform").position
    local b = self.player:Get("Transform").position
    local dx, dz = a.x - b.x, a.z - b.z
    if dx * dx + dz * dz > self.reach * self.reach then
        if self.prompt ~= nil then Prompt(self, nil) end
        return
    end
    local scrap = SaveGames.GetNumber("pack.scrap", 0)
    local components = SaveGames.GetNumber("pack.components", 0)
    local built = SaveGames.GetNumber("crafted.generator_core", 0) > 0
    local affordable = scrap >= self.scrap_cost and components >= self.component_cost
    if built then
        Prompt(self, "Fab bench: the Generator Core is built")
    elseif affordable then
        Prompt(self, string.format("[E] Craft Generator Core  (%d scrap, %d components)", self.scrap_cost, self.component_cost))
        if Input.IsTriggered("Interact") then
            SaveGames.SetNumber("pack.scrap", scrap - self.scrap_cost)
            SaveGames.SetNumber("pack.components", components - self.component_cost)
            SaveGames.SetNumber("crafted.generator_core", 1)
            SaveGames.SaveToSlot("autosave")
            UI.SetText("ScrapText", string.format("SCRAP  %d", scrap - self.scrap_cost))
            UI.SetText("ComponentText", string.format("COMPONENTS  %d", components - self.component_cost))
            Camera.Shake(0.25)
            Sfx("craft")
            self.prompt = nil
            print("Crafted Generator Core")
        end
    else
        Prompt(self, string.format("Fab bench: needs %d scrap and %d components", self.scrap_cost, self.component_cost))
    end
end

return Workbench
