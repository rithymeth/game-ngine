-- Compatibility corpus (M8.1): every kit table a shipped script names still exists with the
-- functions those scripts call. Behavior is covered per module; this pins the names.
local expected = {
    AI = {'CanSee', 'GetKnown', 'GetTarget', 'IsSightReady', 'SetDebugState'},
    Nav = {'FindPath', 'IsReady', 'MoveTowards'},
    Attributes = {'AddBase', 'Define', 'Get', 'GetBase', 'GetMax', 'GetMin', 'Has', 'SetBase'},
    Effects = {'Apply', 'GetActiveCount', 'HasActive', 'Remove', 'RemoveByTag'},
    Abilities = {'CanActivate', 'Cancel', 'Commit', 'End', 'Grant', 'IsActive', 'IsGranted', 'Revoke', 'TryActivate'},
    Inventory = {'Add', 'Configure', 'Count', 'Equip', 'GetEquipped', 'Has', 'Move', 'Remove', 'Split', 'Unequip', 'Use'},
    Interaction = {'CanInteract', 'Find', 'GetPrompt', 'Interact', 'SetEnabled'},
    Quests = {'Abandon', 'Fail', 'GetProgress', 'GetState', 'IsActive', 'IsCompleted', 'Notify', 'Progress', 'Start'},
    UI = {'Exists', 'GetText', 'GetValue', 'SetEnabled', 'SetText', 'SetValue', 'SetVisible', 'ValueChanged', 'WasClicked'},
    Localization = {'Format', 'GetLanguage', 'GetText', 'HasText', 'SetLanguage'},
    SaveGames = {'CreateSaveObject', 'DoesSaveExist', 'LoadFromSlot', 'SaveToSlot', 'DeleteSave'},
    Input = {'GetAxis1D', 'GetAxis2D', 'IsTriggered', 'OnStarted', 'OnTriggered'},
    Physics = {'AddForce', 'AddImpulse', 'GetLinearVelocity', 'Raycast', 'SetLinearVelocity', 'SphereCast'},
    Draw = {'line', 'box', 'sphere', 'text', 'point', 'arrow'},
}
for name, fns in pairs(expected) do
    local t = _G[name]
    assert(type(t) == 'table', name .. ' is missing')
    for _, fn in ipairs(fns) do assert(type(t[fn]) == 'function', name .. '.' .. fn .. ' is missing') end
end
return true
