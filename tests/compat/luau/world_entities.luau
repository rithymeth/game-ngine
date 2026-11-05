-- Compatibility corpus (M8.1): world and entity handles.
local e = world:Spawn()
assert(e:IsValid())
local t = e:Add('Transform')
assert(e:Has('Transform'))
assert(not pcall(function() return e:Has('NoSuchComponent') end)) -- unknown component names are errors
t.position = vector.create(1, 2, 3)
local p = e:Get('Transform').position
assert(p.x == 1 and p.y == 2 and p.z == 3)
assert(type(tostring(e)) == 'string')
local n = 0
for _ in world:EntitiesWith('Transform') do n += 1 end
assert(n >= 1)
e:Remove('Transform')
assert(not e:Has('Transform'))
world:Destroy(e)
assert(not e:IsValid())
assert(not pcall(function() return e:Get('Transform') end)) -- stale handle: a script error, not a crash
return true
