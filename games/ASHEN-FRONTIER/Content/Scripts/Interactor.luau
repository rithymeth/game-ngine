-- Kael's hands (put it on his body, a child of the character: the character has the controller): the nearest thing tagged Salvage within reach can be taken with the Interact action (E, or X on a pad).
-- Taking it removes it from the world and adds what it gives to the pack. The pack lives in SaveGames ("pack.scrap", "pack.components"), so it
-- survives a scene change (a door) and is written to the "autosave" slot at every change: the next run picks up where this one left off.
local Interactor = {}

--@range 0.5 8
--@tooltip How far Kael can reach, metres
Interactor.reach = 3
--@tooltip Components already in the pack at the start (for test scenes)
Interactor.start_components = 0

-- What a salvage entity gives, by the words in its name. Anything else gives scrap.
local YIELDS = {
    { "Crate", "components", 2 },
    { "Pile", "scrap", 3 },
    { "Wreck", "components", 5 },
}

local function Get(kind) return SaveGames.GetNumber("pack." .. kind, 0) end

local function Set(kind, value)
    SaveGames.SetNumber("pack." .. kind, value)
    SaveGames.SaveToSlot("autosave")
end

function Interactor:OnCreate()
    if not SaveGames.HasKey("pack.scrap") and SaveGames.DoesSaveExist("autosave") then SaveGames.LoadFromSlot("autosave") end
    if not SaveGames.HasKey("pack.scrap") then
        SaveGames.SetNumber("pack.scrap", 0)
        SaveGames.SetNumber("pack.components", self.start_components)
    end
    print(string.format("Pack at start: %d scrap, %d components", Get("scrap"), Get("components")))
    self.hud_ready = false
    self.nearest = nil
    self.shown = nil
    self.player = world:EntitiesWith("CharacterMovement")[1]
end

local function Yield(name)
    for _, y in ipairs(YIELDS) do
        if string.find(name, y[1], 1, true) then return y[2], y[3] end
    end
    return "scrap", 1
end

function Interactor:OnUpdate(dt)
    if not self.hud_ready then -- the HUD exists from the first frame: show the pack as it is (it may be carried over)
        self.hud_ready = UI.SetText("ScrapText", string.format("SCRAP  %d", Get("scrap")))
        UI.SetText("ComponentText", string.format("COMPONENTS  %d", Get("components")))
    end
    if self.player == nil then return end
    local here = self.player:Get("Transform").position
    local best, best_d2 = nil, self.reach * self.reach
    for _, e in ipairs(world:EntitiesWith("Tags")) do
        local tags = e:Get("Tags").names
        local salvage = false
        for _, t in ipairs(tags) do if t == "Salvage" then salvage = true end end
        if salvage then
            local p = e:Get("Transform").position
            local dx, dz = p.x - here.x, p.z - here.z
            local d2 = dx * dx + dz * dz
            if d2 < best_d2 then best, best_d2 = e, d2 end
        end
    end
    self.nearest = best
    -- the prompt follows what is in reach
    local shown = best ~= nil and best:Get("EntityName").value or nil
    if shown ~= self.shown then
        -- the HUD exists from the first frame on: until then the call finds nothing and is tried again
        if UI.SetVisible("PromptPanel", shown ~= nil) then
            self.shown = shown
            if shown ~= nil then
                local kind, amount = Yield(shown)
                UI.SetText("PromptText", string.format("[E] Take  (+%d %s)", amount, kind))
            end
        end
    end
    if best ~= nil and Input.IsTriggered("Interact") then
        local name = best:Get("EntityName").value
        local kind, amount = Yield(name)
        Set(kind, Get(kind) + amount)
        world:Destroy(best)
        self.nearest = nil
        self.shown = nil
        UI.SetVisible("PromptPanel", false)
        UI.SetText("ScrapText", string.format("SCRAP  %d", Get("scrap")))
        UI.SetText("ComponentText", string.format("COMPONENTS  %d", Get("components")))
        print(string.format("Salvaged %s: +%d %s (pack: %d scrap, %d components)", name, amount, kind, Get("scrap"), Get("components")))
    end
end

return Interactor
