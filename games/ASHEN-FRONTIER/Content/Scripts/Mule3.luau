-- MULE-3, the cargo robot found in the wreck. Broken at first: repair it (Interact beside it, spending `repair_cost` components from Kael's pack).
-- Then it follows Kael, and Interact beside it again toggles follow / hold. Put it on the robot's entity (no collider: it moves).
local Mule = {}

--@range 0 20
--@tooltip Components the repair takes from the pack
Mule.repair_cost = 3
--@range 0 12
Mule.speed = 5
--@range 0.5 6
--@tooltip How close it comes to Kael, metres
Mule.follow_distance = 2
--@range 0.5 8
--@tooltip How close Kael has to be to talk to it, metres
Mule.reach = 3

local function Sfx(name)
    if not Audio.PlaySound2D("Audio/" .. name .. ".acue") then print("Missing sound: " .. name) end
end

function Mule:OnCreate()
    self.state = "broken" -- broken, follow, hold
    self.player = world:EntitiesWith("CharacterMovement")[1]
    self.prompt = nil
    self.checked = false
end

-- Repaired in an earlier visit (or run)? Asked on the first update, not in OnCreate: the script that loads the autosave (Kael's Interactor) may be
-- created after this one, and until it has, the saved flag is not there to read.
local function Restore(self)
    self.checked = true
    if self.player ~= nil and SaveGames.GetNumber("mule.repaired", 0) > 0 then
        self.state = "follow"
        self.arrived = true
        local t = self.entity:Get("Transform")
        local p = self.player:Get("Transform").position
        t.position = vector.create(p.x + 1.5, p.y, p.z + 1.0)
        print("MULE-3 is with Kael")
    end
end

local function Prompt(self, text)
    if text == self.prompt then return end
    if UI.SetVisible("PromptPanel", text ~= nil) then
        self.prompt = text
        if text ~= nil then UI.SetText("PromptText", text) end
    end
end

function Mule:OnUpdate(dt)
    if self.player == nil then return end
    if not self.checked then Restore(self) end
    local me = self.entity:Get("Transform")
    local p = self.player:Get("Transform").position
    local dx, dz = p.x - me.position.x, p.z - me.position.z
    local dist = math.sqrt(dx * dx + dz * dz)
    local near = dist < self.reach

    if near then
        if self.state == "broken" then
            Prompt(self, string.format("[E] Repair MULE-3  (%d components)", self.repair_cost))
        elseif self.state == "follow" then
            Prompt(self, "[E] MULE-3: hold here")
        else
            Prompt(self, "[E] MULE-3: follow me")
        end
        if Input.IsTriggered("Interact") then
            if self.state == "broken" then
                local have = SaveGames.GetNumber("pack.components", 0)
                if have >= self.repair_cost then
                    SaveGames.SetNumber("pack.components", have - self.repair_cost)
                    SaveGames.SaveToSlot("autosave")
                    UI.SetText("ComponentText", string.format("COMPONENTS  %d", have - self.repair_cost))
                    self.state = "follow"
                    SaveGames.SetNumber("mule.repaired", 1)
                    SaveGames.SaveToSlot("autosave")
                    Sfx("repair")
                    print("MULE-3 repaired: it follows Kael")
                else
                    print("MULE-3 needs components")
                end
            else
                self.state = self.state == "follow" and "hold" or "follow"
                print("MULE-3 " .. self.state)
            end
            self.prompt = nil
        end
    elseif self.prompt ~= nil then
        Prompt(self, nil)
    end

    if self.state == "follow" and dist > self.follow_distance then
        local step = math.min(self.speed * dt, dist - self.follow_distance)
        me.position = vector.create(me.position.x + dx / dist * step, p.y, me.position.z + dz / dist * step)
        if not self.arrived and dist - step <= self.follow_distance + 0.05 then
            self.arrived = true
            print("MULE-3 reached Kael")
        end
    end
end

return Mule
