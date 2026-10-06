#pragma once

#include "aether/core/base.h"

#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aether::loc {

using FormatValue = std::variant<i64, double, std::string>;
using FormatArgs = std::map<std::string, FormatValue>;

// A CLDR plural category: "zero", "one", "two", "few", "many" or "other".
// Rules for the common languages (en and the other one/other languages, pt,
// fr, ja/zh/ko and the others with no plurals, ru/uk, pl, ar); any other
// language follows English. A fractional number is "other" (ru/pl: "many"
// is for whole numbers only).
std::string PluralCategory(std::string_view language, double n);

// Fills `pattern` from `args`:
//   {name}                    the argument as text
//   {count|s}                 "s" unless count's plural category is "one",
//                             so "{count} coin{count|s}" is "1 coin" and "2 coins"
//   {count|one:coin;other:coins}   the text of the category (falling back to
//                             "other", then to nothing)
//   {{ and }}                 a literal brace
// Plurals use the rules of `language`. A problem (an unclosed brace, an
// argument that wasn't given, a plural of a non-number) leaves that part of
// the text as written and adds a line to `errors`; it never throws.
std::string Format(std::string_view pattern, std::string_view language, const FormatArgs& args, std::vector<std::string>* errors = nullptr);

} // namespace aether::loc
