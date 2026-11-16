-- The demo's director: one script that runs the film's clock (the player advances game time by one film frame per frame) and cues what the engine's kits do, so the
-- video is the engine at work and not an animation: the Weather kit rolls the sky from clear to a storm, Time of Day sinks the sun, the Vehicles kit drives the jeep
-- (the script only steers and presses the pedal; the wheels' turning is read back from the physics), the Ropes kit swings a lantern from a gallows in the wind, the
-- Force Fields kit blasts a pile of crates and the Destruction kit breaks them into pieces. It also sets the lens and focus for each shot of the camera path
-- (the path itself is the player's --camera-path). Put it on one entity.
local Director = {}

--@tooltip Seconds of film before the car starts to drive
Director.car_start = 26
--@tooltip Seconds of film when the pile of crates is blown up
Director.blast_time = 35

local function Clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

local function Quat(yaw, pitch)
    local hy, hp = yaw / 2, pitch / 2
    return { math.cos(hy) * math.sin(hp), math.sin(hy) * math.cos(hp), -math.sin(hy) * math.sin(hp), math.cos(hy) * math.cos(hp) }
end

-- What the weather does, and when (seconds of film, the weather, the seconds it takes to get there).
local WEATHER = {
    { 0, "clear", 0 }, { 6, "partly_cloudy", 8 }, { 11, "overcast", 6 }, { 15, "rain", 6 }, { 24, "storm", 6 }, { 40, "rain", 8 },
}

-- The lens for each shot: from this second on, this focal length (mm), this aperture, and what is in focus ("far" leaves the whole picture sharp).
local SHOTS = {
    { 0, 16, 11, "far" }, { 6, 18, 8, "far" }, { 12, 35, 1.8, "lantern" }, { 20, 35, 2.0, "pile" }, { 26, 28, 2.2, "car" },
    { 33, 24, 2.8, "pile" }, { 39, 16, 11, "far" },
}

local function Find(tag)
    for _, e in ipairs(world:EntitiesWith("Tags")) do
        for _, name in ipairs(e:Get("Tags").names) do
            if name == tag then return e end
        end
    end
    return nil
end

function Director:OnCreate()
    self.t = 0
    self.weather_next = 1
    self.shot = 1
    self.blasted = false
    self.blast_light = 0
    self.camera = world:EntitiesWith("Camera")[1]
    self.cine = self.camera ~= nil and self.camera:Get("CineCamera") or nil
    self.car = world:EntitiesWith("Vehicle")[1]
    self.lantern = Find("Lantern")
    self.blast = Find("BlastLight")
    self.wheels = { Find("Wheel0"), Find("Wheel1"), Find("Wheel2"), Find("Wheel3") }
    self.crates = world:EntitiesWith("Destructible")
    local sum, n = vector.create(0, 0, 0), 0
    for _, crate in ipairs(self.crates) do
        sum = sum + crate:Get("Transform").position
        n = n + 1
    end
    self.pile = n > 0 and sum * (1 / n) or vector.create(0, 0, 0)

    -- The world's clock and sky: late afternoon, a seeded weather that only does what this script says.
    TimeOfDay.SetHour(15.6)
    TimeOfDay.SetScale(2.9) -- two and three quarter hours of evening over the film's 48 seconds
    Weather.SetAuto(false, 7)
    Weather.SetCold(false)
    Weather.SetWindDirection(27)
    Weather.Set("clear", 0)
    print(string.format("[demo] %d crates in the pile, %d wheels found, car %s, lantern %s", n, #self.wheels, tostring(self.car ~= nil), tostring(self.lantern ~= nil)))
end

local function CarInput(self, t)
    if t < self.car_start then return 0, 0, 0, 1 end -- parked: the handbrake holds it on the slope
    local u = t - self.car_start
    if u < 6.5 then return math.min(1, u / 1.5) * 0.45, 0.30 * math.sin(u * 1.1), 0, 0 end
    return 0, 0, 1, 0
end

local function Subject(self, name)
    if name == "lantern" and self.lantern ~= nil then return self.lantern:Get("Transform").position end
    if name == "car" and self.car ~= nil then return self.car:Get("Transform").position end
    if name == "pile" then return self.pile end
    return nil
end

function Director:OnUpdate(dt)
    self.t = self.t + dt
    local t = self.t

    -- Weather cues.
    while WEATHER[self.weather_next] ~= nil and t >= WEATHER[self.weather_next][1] do
        local cue = WEATHER[self.weather_next]
        Weather.Set(cue[2], cue[3])
        print(string.format("[demo] t=%.1f weather -> %s over %ds", t, cue[2], cue[3]))
        self.weather_next = self.weather_next + 1
    end

    -- The car: pedals and steering only; the engine, the gearbox, the springs and the grip are the Vehicles kit's.
    if self.car ~= nil then
        local throttle, steer, brake, hand = CarInput(self, t)
        Vehicles.Drive(self.car, throttle, steer, brake, hand)
        local state = self.car:Get("VehicleState")
        for i = 1, (state ~= nil) and 4 or 0 do   -- the state exists from the first fixed step on
            local wheel = self.wheels[i]
            if wheel ~= nil then
                local turn = (i <= 2) and -state.steer_angle or 0 -- the front wheels steer (a right turn is a negative heading)
                wheel:Get("Transform").rotation = Quat(turn, Vehicles.WheelSpin(self.car, i - 1))
            end
        end
    end

    -- The lantern's flame flickers.
    if self.lantern ~= nil then
        self.lantern:Get("PointLight").intensity = 26 * (1 + 0.07 * math.sin(t * 11) + 0.04 * math.sin(t * 23.7))
    end

    -- The blast: a push on everything near the pile, a break of the crates in its middle, a shake and a flash.
    if not self.blasted and t >= self.blast_time then
        self.blasted = true
        local at = self.pile + vector.create(0, 0.6, 0)
        local pushed = Forces.Explode(at, 8.0, 12.0, 0.45)
        local broken = 0
        for _, crate in ipairs(self.crates) do
            if crate:IsValid() and vector.magnitude(crate:Get("Transform").position - self.pile) < 2.8 then
                if Destruction.Shatter(crate) then broken = broken + 1 end
            end
        end
        Camera.Shake(0.8)
        self.blast_light = 1.0
        print(string.format("[demo] t=%.1f explosion: %d bodies pushed, %d crates broken by name", t, pushed, broken))
    end
    if self.blast ~= nil then
        self.blast_light = self.blast_light * math.exp(-5 * dt)
        self.blast:Get("PointLight").intensity = 900 * self.blast_light
        if self.blasted then self.blast:Get("Transform").position = self.pile + vector.create(0, 1.2, 0) end
    end

    -- The lens: each shot's focal length and aperture, with the focus on what it is about.
    while SHOTS[self.shot + 1] ~= nil and t >= SHOTS[self.shot + 1][1] do self.shot = self.shot + 1 end
    if self.cine ~= nil then
        local shot = SHOTS[self.shot]
        local target = Subject(self, shot[4])
        self.cine.focal_length_mm = shot[2]
        self.cine.aperture_f = shot[3]
        if target ~= nil and self.camera ~= nil then
            self.cine.focus_distance = math.max(1, vector.magnitude(target - self.camera:Get("Transform").position))
        else
            self.cine.focus_distance = 400
        end
    end

    -- Where the car is, each second of its run: for aiming the camera at it.
    if self.car ~= nil and t >= 24 and t <= 38 and math.floor(t) ~= self.car_logged then
        self.car_logged = math.floor(t)
        local p = self.car:Get("Transform").position
        print(string.format("[demo] t=%.0f car at %.1f, %.1f, %.1f speed %.1f", t, p.x, p.y, p.z, Vehicles.Speed(self.car)))
    end

    -- A line every few seconds, so a log shows what the engine was doing.
    if math.floor(t) ~= self.logged and math.floor(t) % 4 == 0 then
        self.logged = math.floor(t)
        print(string.format("[demo] t=%.0f hour %.2f %s rain %.2f wind %.1f m/s strikes %d wetness %.2f debris %d speed %.1f",
            t, TimeOfDay.GetHour(), Weather.Current(), Weather.Rain(), Weather.WindSpeed(), Weather.Strikes(), Weather.Wetness(), Destruction.Debris(),
            self.car ~= nil and Vehicles.Speed(self.car) or 0))
    end
end

return Director
