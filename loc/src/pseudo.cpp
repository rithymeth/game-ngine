#include "aether/loc/pseudo.h"

#include "aether/loc/language.h"

#include <cmath>

namespace aether::loc {

namespace {

// Latin-1 accents, which the default editor font has.
const char* Accent(char c) {
    switch (c) {
    case 'a': return "\xC3\xA1"; // á
    case 'c': return "\xC3\xA7"; // ç
    case 'e': return "\xC3\xA9"; // é
    case 'i': return "\xC3\xAD"; // í
    case 'n': return "\xC3\xB1"; // ñ
    case 'o': return "\xC3\xB6"; // ö
    case 'u': return "\xC3\xBA"; // ú
    case 'y': return "\xC3\xBD"; // ý
    case 'A': return "\xC3\x85"; // Å
    case 'E': return "\xC3\x89"; // É
    case 'I': return "\xC3\x8D"; // Í
    case 'O': return "\xC3\x96"; // Ö
    case 'U': return "\xC3\x9A"; // Ú
    case 'C': return "\xC3\x87"; // Ç
    case 'N': return "\xC3\x91"; // Ñ
    default: return nullptr;
    }
}

} // namespace

std::string Pseudo(std::string_view text, const PseudoOptions& o) {
    std::string out;
    usize visible = 0;
    for (usize i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '{') {
            if (i + 1 < text.size() && text[i + 1] == '{') {
                out += "{{";
                ++i;
                continue;
            }
            const usize close = text.find('}', i + 1);
            if (close == std::string_view::npos) { // unclosed: literal, like Format
                out += c;
                ++visible;
                continue;
            }
            out.append(text.substr(i, close - i + 1));
            i = close;
            continue;
        }
        if (c == '}' && i + 1 < text.size() && text[i + 1] == '}') {
            out += "}}";
            ++i;
            continue;
        }
        if (c == '\n' || c == '\r') {
            out += c;
            continue;
        }
        const char* accented = o.accents ? Accent(c) : nullptr;
        if (accented) out += accented;
        else out += c;
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++visible; // count characters, not UTF-8 continuation bytes
    }
    if (visible > 0 && o.expansion > 0.0f) {
        const usize extra = static_cast<usize>(std::ceil(static_cast<f32>(visible) * o.expansion));
        out.append(extra, '~');
    }
    if (o.brackets) out = "[" + out + "]";
    return out;
}

const std::string& PseudoLanguage() {
    static const std::string tag = NormalizeLanguage("qps-ploc");
    return tag;
}

StringTable PseudoTable(const StringTable& table, std::string_view source_language, const PseudoOptions& options) {
    StringTable out = table;
    for (const std::string& key : table.Keys()) {
        if (const std::string* source = table.Find(source_language, key)) out.Set(PseudoLanguage(), key, Pseudo(*source, options));
    }
    return out;
}

} // namespace aether::loc
