-- Drives the Woodland from the engine's own Weather and Time of Day kits, every frame: the sun's height, colour and brightness from the hour and from how much of
-- it the weather lets through, the sky's cloud cover and tint, the distance and height fog, the colour grade, the world wind (which sways the trees and swells the
-- water), the rain that follows the camera, the flash of a lightning strike, and the wind field that pushes the lantern. The kits only keep numbers; this is the part
-- that turns them into a picture. Put it on one entity.
local Driver = {}

--@tooltip Which way the sun's light travels: a heading in the camera's convention (0 faces -Z, positive turns left), degrees
Driver.sun_yaw = 215
--@tooltip How bright the sun is when it is high in a clear sky
Driver.sun_intensity = 5.2
--@tooltip The world height of the sea: the mist pools around and below it
Driver.sea_level = 17

local function Clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end
local function Lerp(a, b, t) return a + (b - a) * t end
local function Mix(a, b, t) return vector.create(Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t)) end

-- The rotation that faces yaw (turning left from -Z) and pitch (negative looks down), both in radians: the camera's convention, which a light shares.
local function Quat(yaw, pitch)
    local hy, hp = yaw / 2, pitch / 2
    return { math.cos(hy) * math.sin(hp), math.sin(hy) * math.cos(hp), -math.sin(hy) * math.sin(hp), math.cos(hy) * math.cos(hp) }
end

function Driver:OnCreate()
    self.sun = world:EntitiesWith("DirectionalLight")[1]
    self.sky = world:EntitiesWith("SkyLight")[1]
    self.post = world:EntitiesWith("PostProcessVolume")[1]
    self.wind = world:EntitiesWith("Wind")[1]
    self.camera = world:EntitiesWith("Camera")[1]
    self.field = world:EntitiesWith("ForceField")[1]
    self.rain_light, self.rain_heavy = nil, nil
    for _, e in ipairs(world:EntitiesWith("ParticleSystem")) do
        local asset = e:Get("ParticleSystem").asset
        if string.find(asset, "RainHeavy") then self.rain_heavy = e elseif string.find(asset, "RainLight") then self.rain_light = e end
    end
    self.rain_on = { light = false, heavy = false }
end

local function SetRain(self, key, entity, on)
    if entity == nil or self.rain_on[key] == on then return end
    self.rain_on[key] = on
    local system = entity:Get("ParticleSystem")
    if on then system:Activate() else system:Deactivate() end
end

function Driver:OnUpdate(dt)
    local clouds, rain, fog, light_scale = Weather.Clouds(), Weather.Rain(), Weather.Fog(), Weather.LightScale()
    local flash = Weather.Lightning()

    -- The sun: its height follows the hour (sunrise 6, sunset 18, the evening kept just above the horizon so the picture stays lit), its colour warms as it sinks,
    -- and the weather decides how much of it gets through.
    local hour = TimeOfDay.GetHour()
    local elevation = math.max(60 * math.sin(math.pi * (hour - 6) / 12), 3)
    local low = Clamp((elevation - 3) / 27, 0, 1)
    if self.sun ~= nil then
        local sun = self.sun:Get("DirectionalLight")
        sun.intensity = self.sun_intensity * (0.18 + 0.82 * low) * light_scale
        local warm = Mix(vector.create(1.0, 0.52, 0.24), vector.create(1.0, 0.94, 0.84), low)
        sun.color = Mix(warm, vector.create(0.78, 0.84, 0.95), Clamp(clouds * 0.7, 0, 0.7))
        self.sun:Get("Transform").rotation = Quat(math.rad(self.sun_yaw), -math.rad(elevation))
    end

    -- The sky: clouds gather, the blue goes grey, and a flash lights the whole thing for a moment.
    if self.sky ~= nil then
        local sky = self.sky:Get("SkyLight")
        sky.cloud_coverage = clouds
        sky.intensity = 0.28 + 0.72 * light_scale + 6.0 * flash
        sky.color = Mix(vector.create(0.55, 0.72, 1.0), vector.create(0.46, 0.52, 0.60), Clamp(clouds * 0.85, 0, 0.85))
    end

    -- Haze, grade and the lightning's exposure kick; the reflections strengthen as the ground gets wet.
    if self.post ~= nil then
        local post = self.post:Get("PostProcessVolume")
        post.fog_density = 0.0007 + 0.006 * fog + 0.0015 * rain
        post.height_fog_density = 0.004 + 0.022 * fog
        post.height_fog_base = self.sea_level
        post.height_fog_falloff = 0.04
        post.saturation = 1.08 - 0.30 * clouds - 0.12 * rain
        post.exposure_compensation = 0.15 * (1 - low) + 1.6 * flash
        post.screen_space_reflections = 0.4 + 0.5 * Weather.Wetness()
        post.vignette = 0.18 + 0.12 * rain
    end

    -- The world wind: it sways the plants and swells the water, so a storm shakes the trees and lifts the lake.
    if self.wind ~= nil then
        local wind = self.wind:Get("Wind")
        wind.strength = 0.5 + Weather.WindSpeed() * 0.07
        wind.direction_degrees = Weather.WindDirection()
    end

    -- The wind field round the lantern pushes it (and its rope) along the same wind.
    if self.field ~= nil then
        local field = self.field:Get("ForceField")
        local yaw = math.rad(Weather.WindDirection())
        field.direction = vector.create(math.sin(yaw), 0, math.cos(yaw))
        field.strength = 0.22 * Weather.WindSpeed()
    end

    -- Rain: a box of streaks high above the camera, light or heavy by how hard it rains.
    if self.camera ~= nil then
        local above = self.camera:Get("Transform").position + vector.create(0, 14, 0)
        for _, e in ipairs({ self.rain_light, self.rain_heavy }) do
            if e ~= nil then e:Get("Transform").position = above end
        end
    end
    local heavy = rain > 0.75
    SetRain(self, "heavy", self.rain_heavy, heavy)
    SetRain(self, "light", self.rain_light, rain > 0.12 and not heavy)
end

return Driver
