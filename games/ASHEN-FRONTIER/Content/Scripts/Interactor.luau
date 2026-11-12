-- Kael's hands (put it on his body, a child of the character: the character has the controller): the nearest thing tagged Salvage within reach can be taken with the Interact action (E, or X on a pad).
-- Taking it removes it from the world and adds what it gives to the pack (self.pack, by resource). Later systems (crafting, the HUD, the save)
-- read the pack from here.
local Interactor = {}

--@range 0.5 8
--@tooltip How far Kael can reach, metres
Interactor.reach = 3

-- What a salvage entity gives, by the words in its name. Anything else gives scrap.
local YIELDS = {
    { "Crate", "components", 2 },
    { "Pile", "scrap", 3 },
    { "Wreck", "components", 5 },
}

function Interactor:OnCreate()
    self.pack = { scrap = 0, components = 0 }
    self.nearest = nil
    self.player = world:EntitiesWith("CharacterMovement")[1]
end

local function Yield(name)
    for _, y in ipairs(YIELDS) do
        if string.find(name, y[1], 1, true) then return y[2], y[3] end
    end
    return "scrap", 1
end

function Interactor:OnUpdate(dt)
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
    if best ~= nil and Input.IsTriggered("Interact") then
        local name = best:Get("EntityName").value
        local kind, amount = Yield(name)
        self.pack[kind] = self.pack[kind] + amount
        world:Destroy(best)
        self.nearest = nil
        print(string.format("Salvaged %s: +%d %s (pack: %d scrap, %d components)", name, amount, kind, self.pack.scrap, self.pack.components))
    end
end

return Interactor
