-- Stardust Hop: the player's script. WASD or the left stick runs, the mouse or the right stick turns the
-- camera, Space or the A button jumps, R starts over. Collect the coins, touch the flags to move your respawn
-- point, and reach the golden pad at the end. There is no text on screen in this engine build, so the score is
-- shown in the world: a row of gems floats in the top-left of the view (lit gold when you hold that coin).
local Hopper = {}

--@range 0 30
--@tooltip Running speed, metres per second
Hopper.speed = 6.5
--@range 0 1
--@tooltip Degrees of camera turn per pixel of mouse movement
Hopper.look_speed = 0.12
--@range 2 20
--@tooltip How far the camera sits behind the player
Hopper.distance = 7
--@range 0 5
--@tooltip How high above the feet the camera looks
Hopper.height = 1.7
--@range 0 20
Hopper.jump_speed = 6.2
--@range -100 0
--@tooltip Falling below this height respawns you
Hopper.kill_y = -10

local function Quat(yaw, pitch)
    local hy, hp = yaw / 2, pitch / 2
    return {
        math.cos(hy) * math.sin(hp), math.sin(hy) * math.cos(hp),
        -math.sin(hy) * math.sin(hp), math.cos(hy) * math.cos(hp),
    }
end

local function TagsOf(entity)
    local ok, tags = pcall(function() return entity:Get("Tags").names end)
    return ok and tags or {}
end

local function Play(entity, cue)
    if entity == nil or not entity:IsValid() then return end
    local source = entity:Get("AudioSource")
    if cue ~= nil and source.cue ~= cue then source:SetCue(cue) end
    source:Play()
end

local FAR = vector.create(0, -500, 0)

function Hopper:OnCreate()
    local transform = self.entity:Get("Transform")
    self.yaw, self.pitch = 0, -0.28
    self.start = transform.position
    self.spawn = transform.position
    self.camera = world:EntitiesWith("Camera")[1]
    self.character = self.entity:Get("CharacterMovement")
    self.character.walk_speed = self.speed
    self.character.run_speed = self.speed
    self.character.jump_velocity = self.jump_speed

    self.coins, self.checkpoints, self.pips_on, self.pips_off, self.confetti = {}, {}, {}, {}, {}
    self.sfx = {}
    self.goal = nil
    for _, entity in ipairs(world:EntitiesWith("Tags")) do
        for _, tag in ipairs(TagsOf(entity)) do
            if tag == "Coin" then
                table.insert(self.coins, {entity = entity, home = entity:Get("Transform").position, taken = false})
            elseif tag == "Checkpoint" then
                table.insert(self.checkpoints, {entity = entity, position = entity:Get("Transform").position, reached = false})
            elseif tag == "Goal" then
                self.goal = entity
            elseif tag == "PipOn" then
                table.insert(self.pips_on, entity)
            elseif tag == "PipOff" then
                table.insert(self.pips_off, entity)
            elseif tag == "Confetti" then
                table.insert(self.confetti, {entity = entity, velocity = vector.zero, life = 0})
            elseif string.sub(tag, 1, 3) == "Sfx" then
                self.sfx[tag] = entity
            end
        end
    end
    -- Keep the pips in the same order as the coins' tags were found.
    self.total = #self.coins
    self.collected = 0
    self.time = 0
    self.won = false
    self.win_time = 0
    self.jump_cooldown = 0
    self.fall_cooldown = 0
    print("Stardust Hop: " .. self.total .. " coins to find")
end

function Hopper:Reset()
    self.collected = 0
    self.won = false
    self.win_time = 0
    for _, coin in ipairs(self.coins) do
        coin.taken = false
        coin.entity:Get("Transform").position = coin.home
    end
    for _, checkpoint in ipairs(self.checkpoints) do checkpoint.reached = false end
    for _, piece in ipairs(self.confetti) do
        piece.life = 0
        piece.entity:Get("Transform").position = FAR
    end
    self.spawn = self.start
    self.yaw, self.pitch = 0, -0.28
    self:Teleport(self.start)
end

function Hopper:Teleport(position)
    self.entity:Get("Transform").position = position
end

function Hopper:Burst()
    -- Confetti flies out of the goal pad when you win.
    local origin = self.goal:Get("Transform").position + vector.create(0, 5.4, 0)
    for i, piece in ipairs(self.confetti) do
        local angle = i * 2.399963 -- the golden angle spreads the pieces evenly
        local lift = 5 + (i % 5) * 1.5
        piece.velocity = vector.create(math.cos(angle) * (2 + (i % 4)), lift, math.sin(angle) * (2 + (i % 4)))
        piece.life = 3.2
        piece.entity:Get("Transform").position = origin
    end
end

function Hopper:UpdateCamera(player_position)
    if self.camera == nil or not self.camera:IsValid() then return end
    local sy, cy = math.sin(self.yaw), math.cos(self.yaw)
    local sp, cp = math.sin(self.pitch), math.cos(self.pitch)
    local forward = vector.create(-sy * cp, sp, -cy * cp)
    local target = player_position + vector.create(0, self.height, 0)
    local camera_transform = self.camera:Get("Transform")
    camera_transform.position = target - forward * self.distance
    camera_transform.rotation = Quat(self.yaw, self.pitch)

    -- The gem row: tied to the camera so it stays in the corner of the view.
    local right = vector.create(cy, 0, -sy)
    local up = vector.cross(right, forward)
    local origin = camera_transform.position + forward * 3.0 + right * -1.55 + up * 1.05
    for i = 1, self.total do
        local spot = origin + right * ((i - 1) * 0.17)
        local lit = self.pips_on[i]
        local dark = self.pips_off[i]
        if lit ~= nil then lit:Get("Transform").position = (i <= self.collected) and spot or FAR end
        if dark ~= nil then dark:Get("Transform").position = (i > self.collected) and spot or FAR end
    end
end

function Hopper:OnUpdate(dt)
    self.time += dt
    self.jump_cooldown = math.max(0, self.jump_cooldown - dt)
    self.fall_cooldown = math.max(0, self.fall_cooldown - dt)
    local transform = self.entity:Get("Transform")

    if Input.IsTriggered("Restart") then self:Reset() end

    -- Camera orbit and movement relative to where the camera looks.
    local look = Input.GetAxis2D("Look")
    self.yaw = self.yaw - math.rad(look.x * self.look_speed)
    self.pitch = math.clamp(self.pitch - math.rad(look.y * self.look_speed), -1.2, 0.5)
    local move = Input.GetAxis2D("Move")
    if vector.magnitude(move) > 1 then move = vector.normalize(move) end
    local sy, cy = math.sin(self.yaw), math.cos(self.yaw)
    local vx = -sy * move.y + cy * move.x
    local vz = -cy * move.y - sy * move.x
    self.character.input = vector.create(vx, 0, vz)
    local jump = Input.IsTriggered("Jump")
    self.character.jump_requested = jump
    if jump and self.jump_cooldown <= 0 then
        self.jump_cooldown = 0.45
        Play(self.sfx.SfxJump)
    end
    if vx ~= 0 or vz ~= 0 then
        local heading = math.atan2(-vx, -vz)
        transform.rotation = {0, math.sin(heading / 2), 0, math.cos(heading / 2)}
    end

    local position = transform.position

    -- Coins spin and bob; touching one takes it.
    local spin = self.time * 2.4
    for _, coin in ipairs(self.coins) do
        if not coin.taken then
            local coin_transform = coin.entity:Get("Transform")
            coin_transform.rotation = {0, math.sin(spin / 2), 0, math.cos(spin / 2)}
            coin_transform.position = coin.home + vector.create(0, math.sin(self.time * 2 + coin.home.x) * 0.12, 0)
            local offset = position + vector.create(0, 0.9, 0) - coin.home
            if vector.magnitude(offset) < 1.1 then
                coin.taken = true
                self.collected += 1
                coin_transform.position = FAR
                Play(self.sfx.SfxCoin)
                print("Coins " .. self.collected .. "/" .. self.total)
            end
        end
    end

    -- Flags: touch one and you come back to it when you fall.
    for _, checkpoint in ipairs(self.checkpoints) do
        if not checkpoint.reached and vector.magnitude(position - checkpoint.position) < 2.2 then
            checkpoint.reached = true
            self.spawn = checkpoint.position + vector.create(1.2, 0.2, 0)
            Play(self.sfx.SfxCheckpoint)
            print("Checkpoint reached")
        end
    end

    -- Falling off the world.
    if position.y < self.kill_y and self.fall_cooldown <= 0 then
        self.fall_cooldown = 1
        Play(self.sfx.SfxFall)
        self:Teleport(self.spawn)
        print("Respawn")
    end

    -- The goal.
    if self.goal ~= nil and not self.won then
        local goal_position = self.goal:Get("Transform").position
        local flat = vector.create(position.x - goal_position.x, 0, position.z - goal_position.z)
        if vector.magnitude(flat) < 2.4 and math.abs(position.y - goal_position.y) < 1.6 then
            self.won = true
            self.win_time = 0
            Play(self.sfx.SfxWin)
            self:Burst()
            print("You win with " .. self.collected .. "/" .. self.total .. " coins in " .. string.format("%.1f", self.time) .. "s")
        end
    end

    if self.won then
        self.win_time += dt
        for _, piece in ipairs(self.confetti) do
            if piece.life > 0 then
                piece.life -= dt
                piece.velocity = piece.velocity + vector.create(0, -9 * dt, 0)
                local confetti_transform = piece.entity:Get("Transform")
                confetti_transform.position = confetti_transform.position + piece.velocity * dt
                if piece.life <= 0 then confetti_transform.position = FAR end
            end
        end
        if self.win_time > 7 then self:Reset() end
    end

    self:UpdateCamera(transform.position)
end

return Hopper
