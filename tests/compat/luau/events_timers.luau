-- Compatibility corpus (M8.1): Event and Timer as written against engine 0.27.
local hits, last = 0, nil
local ev = Event.new()
local conn = ev:Connect(function(x) hits += 1; last = x end)
ev:Fire(7)
assert(hits == 1 and last == 7)
assert(conn:IsConnected() and conn:Disconnect() and not conn:IsConnected())
ev:Fire(8)
assert(hits == 1)

once, every = 0, 0
Timer.After(0.5, function() once += 1 end)
repeating = Timer.Every(0.25, function() every += 1 end)
function CompatCheck()
    -- the test ticks 1 s of game time before calling this
    assert(once == 1, "Timer.After fired " .. once)
    assert(every == 4, "Timer.Every fired " .. every)
    return repeating:Cancel() and not repeating:Cancel()
end
return true
