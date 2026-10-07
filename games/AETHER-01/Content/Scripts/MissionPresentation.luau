-- Temporary sound feedback and the archive reveal use existing engine components.
local Presentation = {}

local function Play(entity, cue)
    if entity == nil then return end
    local source = entity:Get("AudioSource")
    if source.cue ~= cue then source:SetCue(cue) end
    source:Play()
end

function Presentation:OnCreate()
    self.player, self.damage, self.warning, self.archive = nil, nil, nil, nil
    self.enemies, self.attacks = {}, {}
    for _, entity in ipairs(world:EntitiesWith("Tags")) do
        for _, tag in ipairs(entity:Get("Tags").names) do
            if tag == "Player" then self.player = entity end
            if tag == "DamageAudio" then self.damage = entity end
            if tag == "WarningAudio" then self.warning = entity end
            if tag == "ArchiveTerminal" then self.archive = entity end
            if tag == "Sentinel" or tag == "Hunter" or tag == "Warden" then table.insert(self.enemies, entity) end
        end
    end
    if self.player == nil then return end
    self.ammo = Attributes.Get(self.player, "Ammo")
    self.health = Attributes.Get(self.player, "Health")
    self.reloading = Attributes.Get(self.player, "Reloading")
    self.weapon = Attributes.Get(self.player, "WeaponIndex")
    self.started = false
    Attributes.Define(self.player, "ArchiveTime", 0, 0, 8)
    Attributes.Define(self.player, "ArchivePlaying", 0, 0, 1)
end

function Presentation:OnUpdate(dt)
    if self.player == nil then return end
    local ammo, health = Attributes.Get(self.player, "Ammo"), Attributes.Get(self.player, "Health")
    local weapon = Attributes.Get(self.player, "WeaponIndex")
    local reloading = Attributes.Get(self.player, "Reloading")
    -- Weapon switches can change the displayed magazine without firing.
    if ammo < self.ammo and weapon == self.weapon then
        Play(self.entity, "Audio/" .. ({"rifle", "pistol", "shotgun"})[weapon] .. ".acue")
    end
    if reloading > 0 and self.reloading == 0 then Play(self.entity, "Audio/reload.acue") end
    if health < self.health then Play(self.damage, "Audio/hurt.acue") end
    self.ammo, self.health, self.reloading, self.weapon = ammo, health, reloading, weapon
    for _, enemy in ipairs(self.enemies) do
        local attack = Attributes.Get(enemy, "AttackState")
        if attack > 0 and (self.attacks[enemy:Guid()] or 0) == 0 then Play(self.warning, "Audio/warning.acue") end
        self.attacks[enemy:Guid()] = attack
    end
    if self.archive ~= nil then
        local sequence = self.archive:Get("SequenceComponent")
        if Attributes.Get(self.player, "MissionStage") == 5 then
            if not self.started then sequence:Play(); self.started = true end
            Attributes.SetBase(self.player, "ArchiveTime", sequence:GetTime())
            Attributes.SetBase(self.player, "ArchivePlaying", sequence:IsPlaying() and 1 or 0)
        elseif self.started then
            sequence:Stop()
            self.started = false
            Attributes.SetBase(self.player, "ArchiveTime", 0)
            Attributes.SetBase(self.player, "ArchivePlaying", 0)
        end
    end
end

return Presentation
