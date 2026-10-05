#include "aether/ui/bidi.h"

#include <algorithm>

namespace aether::ui {

namespace {

enum class Kind : u8 { L, R, EN, Neutral };

bool IsHebrewOrArabicRange(u32 c) {
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF) || (c >= 0x10800 && c <= 0x10FFF) ||
           (c >= 0x1E800 && c <= 0x1EFFF);
}

bool IsDigit(u32 c) { return (c >= '0' && c <= '9') || (c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9); }

Kind Classify(u32 c) {
    if (IsDigit(c)) return Kind::EN;
    if (IsRightToLeft(c)) return Kind::R;
    // Letters of other scripts (anything past ASCII that isn't punctuation or a space) and ASCII letters are strong left-to-right.
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return Kind::L;
    if (c >= 0x00C0 && !(c >= 0x2000 && c <= 0x206F) && !(c >= 0x3000 && c <= 0x303F) && !(c >= 0xFF00 && c <= 0xFF0F)) return Kind::L;
    return Kind::Neutral;
}

u32 Mirrored(u32 c) {
    switch (c) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    case '<': return '>';
    case '>': return '<';
    case 0x00AB: return 0x00BB; // « »
    case 0x00BB: return 0x00AB;
    default: return c;
    }
}

} // namespace

bool IsRightToLeft(u32 c) {
    if (!IsHebrewOrArabicRange(c)) return false;
    return !IsDigit(c); // Arabic-Indic digits read left to right
}

bool HasRightToLeft(const std::vector<u32>& text) {
    return std::any_of(text.begin(), text.end(), [](u32 c) { return IsRightToLeft(c); });
}

bool BaseDirectionIsRtl(const std::vector<u32>& text) {
    for (const u32 c : text) {
        const Kind k = Classify(c);
        if (k == Kind::R) return true;
        if (k == Kind::L) return false;
    }
    return false;
}

std::vector<u32> ReorderVisual(const std::vector<u32>& logical) {
    if (!HasRightToLeft(logical)) return logical;
    const usize n = logical.size();
    const bool base_rtl = BaseDirectionIsRtl(logical);
    const u8 base = base_rtl ? 1 : 0;
    std::vector<Kind> kind(n);
    for (usize i = 0; i < n; ++i) kind[i] = Classify(logical[i]);
    // Neutrals: the direction on both sides if it is the same (digits count as left-to-right), else the base.
    const auto strong_dir = [&](Kind k) { return k == Kind::R ? 1 : 0; };
    std::vector<u8> level(n, base);
    for (usize i = 0; i < n; ++i) {
        if (kind[i] == Kind::R) {
            level[i] = 1;
        } else if (kind[i] == Kind::L || kind[i] == Kind::EN) {
            level[i] = base_rtl ? 2 : 0; // left-to-right inside a right-to-left line sits one level up
        }
    }
    for (usize i = 0; i < n; ++i) {
        if (kind[i] != Kind::Neutral) continue;
        usize a = i, b = i;
        while (a > 0 && kind[a - 1] == Kind::Neutral) --a;
        while (b + 1 < n && kind[b + 1] == Kind::Neutral) ++b;
        const bool has_before = a > 0, has_after = b + 1 < n;
        u8 resolved = base; // the level of the base direction itself
        if (has_before && has_after && strong_dir(kind[a - 1]) == strong_dir(kind[b + 1])) {
            // Between two letters of one direction: that direction's level (left-to-right text in a right-to-left line sits one level up).
            resolved = strong_dir(kind[a - 1]) == 1 ? 1 : (base_rtl ? 2 : 0);
        }
        for (usize k = a; k <= b; ++k) level[k] = resolved;
        i = b;
    }
    // Reverse runs from the deepest level down to the lowest odd one.
    std::vector<u32> out = logical;
    std::vector<usize> order(n);
    for (usize i = 0; i < n; ++i) order[i] = i;
    u8 highest = 0, lowest_odd = 255;
    for (const u8 l : level) {
        highest = std::max(highest, l);
        if (l & 1) lowest_odd = std::min(lowest_odd, l);
    }
    for (int l = highest; l >= static_cast<int>(lowest_odd) && l > 0; --l) {
        for (usize i = 0; i < n;) {
            if (level[order[i]] < l) {
                ++i;
                continue;
            }
            usize j = i;
            while (j < n && level[order[j]] >= l) ++j;
            std::reverse(order.begin() + static_cast<std::ptrdiff_t>(i), order.begin() + static_cast<std::ptrdiff_t>(j));
            i = j;
        }
    }
    for (usize i = 0; i < n; ++i) {
        const usize src = order[i];
        out[i] = (level[src] & 1) ? Mirrored(logical[src]) : logical[src];
    }
    return out;
}

} // namespace aether::ui
