-- Compatibility corpus (M8.1): pure GameplayTags helpers.
assert(GameplayTags.Matches('Damage.Fire.Burning', 'Damage.Fire'))
assert(not GameplayTags.MatchesExact('Damage.Fire.Burning', 'Damage.Fire'))
assert(GameplayTags.GetDepth('A.B.C') == 3)
assert(GameplayTags.GetParent('A.B.C') == 'A.B')
assert(GameplayTags.IsValid('A.B') and not GameplayTags.IsValid(''))
return true
