#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace aether::loc {

// Localized assets (Phase 29 step 6, §29.6): a language's variant of an asset
// is the same path with the language before the extension, so
// `Textures/logo.png` has `Textures/logo.fr.png` and `Textures/logo.pt-BR.png`.
// The variants are ordinary assets (they cook and ship like any other); only
// the lookup knows the naming.

// The paths to try, most specific first: for "pt-BR" and `Textures/logo.png`:
// logo.pt-BR.png, logo.pt.png, then the base logo.png (the default language's,
// so a variant for `default_language` itself isn't tried). Always ends with `path`.
std::vector<std::string> LocalizedCandidates(std::string_view path, std::string_view language, std::string_view default_language = "en");

// If `path` looks like a language variant (its last name part before the
// extension is a language tag: `logo.fr.png`, `logo.pt-BR.png`), the base path
// (`logo.png`) and the tag; else false. Needs a 2 or 3 letter lower-case language,
// optionally with subtags, so `icon.large.png` and `v1.2.png` aren't variants.
bool SplitVariant(std::string_view path, std::string* base = nullptr, std::string* language = nullptr);

// The variants in `paths` whose base path isn't among them (a translated asset
// with nothing to fall back to, or a typo in the language), for a warning.
std::vector<std::string> OrphanedVariants(const std::vector<std::string>& paths);

} // namespace aether::loc
