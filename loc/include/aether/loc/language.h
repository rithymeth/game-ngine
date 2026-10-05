#pragma once

#include "aether/core/base.h"

#include <string>
#include <string_view>
#include <vector>

namespace aether::loc {

// "pt_br", "PT-br" and "pt-BR" are all "pt-BR": the language in lower case,
// then each further subtag as written by BCP 47 (a 2-letter region in upper
// case, a 4-letter script like "Hans" capitalized). `_` becomes `-`.
std::string NormalizeLanguage(std::string_view tag);

// The languages to try, most specific first, ending at `default_language`:
// "pt-BR" gives {pt-BR, pt, en}; "zh-Hans-CN" gives {zh-Hans-CN, zh-Hans, zh, en}.
// No entry repeats, and an empty or unknown tag is just {default_language}.
std::vector<std::string> FallbackChain(std::string_view language, std::string_view default_language = "en");

} // namespace aether::loc
