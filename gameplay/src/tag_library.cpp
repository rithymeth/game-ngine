#include "aether/gameplay/tag_library.h"

#include "aether/gameplay/gameplay_tag.h"

namespace aether::gas {

bool GameplayTags::IsValid(const std::string& tag) { return GameplayTag::ValidName(tag); }
bool GameplayTags::Matches(const std::string& tag, const std::string& parent) { return GameplayTag::Make(tag).Matches(GameplayTag::Make(parent)); }
bool GameplayTags::MatchesExact(const std::string& tag, const std::string& other) { return GameplayTag::Make(tag).MatchesExact(GameplayTag::Make(other)); }
std::string GameplayTags::GetParent(const std::string& tag) { return GameplayTag::Make(tag).Parent().name; }
i32 GameplayTags::GetDepth(const std::string& tag) { return GameplayTag::Make(tag).Depth(); }

} // namespace aether::gas
