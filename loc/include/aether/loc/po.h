#pragma once

#include "aether/loc/string_table.h"

#include <string>
#include <string_view>
#include <vector>

namespace aether::loc {

// GNU gettext .po files, the format translation tools speak (§29.4). One file
// is one language. Each entry is
//
//   msgctxt "<key>"
//   msgid "<source text>"
//   msgstr "<translation>"
//
// so the key identifies the string and the source text is what the translator
// reads. Plural forms (msgid_plural, msgstr[n]) are not used: a plural is written
// inside the string with the §29.1 message format ({count|one:..;other:..}).

// The file for `language`: a header (UTF-8, `Language:`), then every key in the
// table, its source text from `source_language` (the key if it has none) and its
// translation in `language` (empty if not translated yet). Keys sorted.
std::string ExportPo(const StringTable& table, std::string_view language, std::string_view source_language, const std::string& project_name = {});

// Sets the translations a .po file holds into `table` as `language`. Skips an
// entry with an empty msgstr, a fuzzy one (`#, fuzzy`, with a warning in
// `errors`), the header and obsolete (`#~`) entries. An entry with no msgctxt
// uses its msgid as the key (a .po from another tool). Accepts the split
// `""` continuation form and the escapes \\ \" \n \t \r. Returns false if a
// line isn't valid .po (the line number is in `errors`; entries before it
// were applied).
bool ImportPo(StringTable& table, std::string_view language, std::string_view po, std::vector<std::string>* errors = nullptr);

// The `Language:` of a .po file's header ("" if it has none).
std::string PoLanguage(std::string_view po);

} // namespace aether::loc
