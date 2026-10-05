#pragma once

#include "aether/core/base.h"
#include "aether/reflection/reflection.h"

#include <string>

// Blueprint function library for text (Phase 29 step 2, §29.2): Get Text,
// Format Text, Set and Get Language, Has Text. Each acts on
// Localization::Active() and, without one, returns the fallback (or the key),
// so a missing service never blanks a string. The same functions are Luau's
// `Localization` table (scripting/loc_api.h).

namespace aether::loc {

struct Localize {
    // The key's text in the current language, else `fallback`, else the key.
    static std::string GetText(const std::string& key, const std::string& fallback);
    // The same with one argument filled in: "{count} coin{count|s}" with
    // name "count" and value 3 is "3 coins".
    static std::string FormatInt(const std::string& key, const std::string& fallback, const std::string& name, i32 value);
    static std::string FormatString(const std::string& key, const std::string& fallback, const std::string& name, const std::string& value);
    static bool HasText(const std::string& key);
    // Changes the language (the game's saved setting follows).
    static void SetLanguage(const std::string& language);
    static std::string GetLanguage();
};

} // namespace aether::loc

AETHER_REFLECT(aether::loc::Localize, 1,
    AETHER_METHOD(GetText, Fn_BlueprintCallable | Fn_Pure, {"key", "default"}),
    AETHER_METHOD(FormatInt, Fn_BlueprintCallable | Fn_Pure, {"key", "default", "name", "value"}),
    AETHER_METHOD(FormatString, Fn_BlueprintCallable | Fn_Pure, {"key", "default", "name", "value"}),
    AETHER_METHOD(HasText, Fn_BlueprintCallable | Fn_Pure, {"key"}),
    AETHER_METHOD(SetLanguage, Fn_BlueprintCallable, {"language"}),
    AETHER_METHOD(GetLanguage, Fn_BlueprintCallable | Fn_Pure)
)
