-- The fab bench: where salvage becomes a part. Walk up to it and Interact to craft a Generator Core. What it costs is the "GeneratorCore" recipe
-- (Content/Recipes), taken from Kael's pack by the engine's Crafting kit, all or nothing; "crafted.generator_core" is set, which the generator and the
-- objective line read.
local Workbench = {}

--@range 0.5 8
Workbench.reach = 3

local RECIPE = "GeneratorCore"

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
    local built = SaveGames.GetNumber("crafted.generator_core", 0) > 0
    if built then
        Prompt(self, "Fab bench: the Generator Core is built")
    elseif Crafting.CanCraft(self.player, RECIPE) then
        Prompt(self, string.format("[E] Craft %s  (%s)", Crafting.GetTitle(RECIPE), Crafting.Describe(RECIPE)))
        if Input.IsTriggered("Interact") then
            local result = Crafting.Craft(self.player, RECIPE)
            if result == "ok" then
                SaveGames.SetNumber("crafted.generator_core", 1)
                Achievements.Unlock("Fabricator")
                SaveGames.CaptureWorld() -- the pack is saved with Kael
                SaveGames.SaveToSlot("autosave")
                UI.SetText("ScrapText", string.format("SCRAP  %d", Inventory.Count(self.player, "Scrap")))
                UI.SetText("ComponentText", string.format("COMPONENTS  %d", Inventory.Count(self.player, "Component")))
                Camera.Shake(0.25)
                Sfx("craft")
                self.prompt = nil
                print("Crafted Generator Core")
            else
                print("Could not craft the Generator Core: " .. result)
            end
        end
    else
        Prompt(self, "Fab bench: needs " .. Crafting.Describe(RECIPE))
    end
end

return Workbench
