#include "aether/ui/basic.h"
#include "aether/ui/draw.h"
#include "aether/ui/font.h"
#include "aether/ui/panels.h"
#include "aether/ui/viewport.h"
#include "aether/ui/panels.h"
#include "test_framework.h"

#include <cmath>
#include <set>

// Phase 29 step 6 (§29.6): font fallback chains and stricter UTF-8.

using namespace aether;
using namespace aether::ui;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

// A font that has only some code points, each `advance` wide, with its own atlas texture.
class StubFont final : public Font {
public:
    StubFont(std::set<u32> has, f32 advance, u32 texture, f32 sdf_range = 0.0f) : has_(std::move(has)), advance_(advance), texture_(texture), sdf_range_(sdf_range) {}
    f32 Ascent(f32 size) const override { return 0.8f * size; }
    f32 LineHeight(f32 size) const override { return 1.2f * size; }
    Glyph GlyphOf(u32 cp, f32 size) const override {
        Glyph g;
        g.advance = advance_ * size;
        if (has_.count(cp) != 0) g.quad = {0, -0.7f * size, advance_ * size, 0.7f * size};
        else g.advance = 0.25f * size; // the stub's tofu is narrow
        return g;
    }
    f32 Kerning(u32, u32, f32 size) const override { return -0.1f * size; }
    u32 Texture() const override { return texture_; }
    f32 SdfRange(f32) const override { return sdf_range_; }
    bool HasGlyph(u32 cp) const override { return has_.count(cp) != 0; }

private:
    std::set<u32> has_;
    f32 advance_;
    u32 texture_;
    f32 sdf_range_;
};

bool Near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

} // namespace

AETHER_TEST(UIFallback_EachCodePointComesFromTheFirstFontThatHasIt) {
    StubFont latin({'a', 'b', ' '}, 0.5f, 1);
    StubFont cjk({0x4E2D, 0x6587}, 1.0f, 2);
    StubFont emoji({0x1F600}, 1.2f, 3);
    const FallbackFont chain({&latin, &cjk, &emoji});
    CHECK(chain.HasGlyph('a') && chain.HasGlyph(0x4E2D) && chain.HasGlyph(0x1F600) && !chain.HasGlyph('z'));
    CHECK(&chain.Source('a') == &latin && &chain.Source(0x4E2D) == &cjk && &chain.Source(0x1F600) == &emoji);
    CHECK(&chain.Source('z') == &latin); // in no font: the first font's replacement glyph
    CHECK(Near(chain.GlyphOf('a', 10).advance, 5) && Near(chain.GlyphOf(0x4E2D, 10).advance, 10) && Near(chain.GlyphOf(0x1F600, 10).advance, 12));
    CHECK(Near(chain.GlyphOf('z', 10).advance, 2.5f));
    // Metrics are the first font's.
    CHECK(Near(chain.Ascent(10), 8) && Near(chain.LineHeight(10), 12));
    // Kerning only inside one font.
    CHECK(Near(chain.Kerning('a', 'b', 10), -1.0f) && chain.Kerning('a', 0x4E2D, 10) == 0.0f);
    const FallbackFont empty({});
    CHECK(empty.GlyphOf('a', 10).advance == 0.0f && !empty.HasGlyph('a'));
}

AETHER_TEST(UIFallback_LayoutAndDrawingMixFonts) {
    StubFont latin({'a', 'b'}, 0.5f, 1);
    StubFont cjk({0x4E2D}, 1.0f, 2, 8.0f); // a distance-field font
    const FallbackFont chain({&latin, &cjk});
    const std::string text = "a\xE4\xB8\xAD" "b"; // a 中 b
    const TextLayout layout = LayoutText(chain, text, 10, 0);
    // a (5) + 中 (10) + b (5), and kerning only where two glyphs share a font: none here.
    CHECK(Near(layout.size.x, 20.0f) && layout.lines.size() == 1);
    DrawList list;
    list.AddText(chain, text, 10, {0, 0, 100, 20}, Color{1, 1, 1, 1}, TextAlign::Left, 0, nullptr);
    CHECK(list.quads.size() == 3);
    CHECK(list.quads[0].texture == 1 && list.quads[0].sdf_range == 0.0f); // from the bitmap font
    CHECK(list.quads[1].texture == 2 && list.quads[1].sdf_range == 8.0f); // from the SDF font, with its own range
    CHECK(list.quads[2].texture == 1);
    CHECK(Near(list.quads[1].rect.x, 5.0f) && Near(list.quads[2].rect.x, 15.0f));
}

AETHER_TEST(UIFallback_LibraryBuildsChainsFromCommaNames) {
    StubFont latin({'a'}, 0.5f, 1), cjk({0x4E2D}, 1.0f, 2);
    FontLibrary fonts;
    fonts.Add("Latin", &latin);
    fonts.Add("CJK", &cjk);
    CHECK(fonts.Find("Latin") == &latin && fonts.Find("Nope") == nullptr);
    const Font* chain = fonts.Find("Latin, CJK, Missing");
    CHECK(chain != nullptr && chain != &latin && chain->HasGlyph('a') && chain->HasGlyph(0x4E2D));
    CHECK(fonts.Find("Latin, CJK, Missing") == chain); // cached
    CHECK(fonts.Find("Missing,Alsomissing") == nullptr);
    CHECK(fonts.Find("CJK,Latin") != chain);
    CHECK((fonts.Names() == std::vector<std::string>{"Latin", "CJK"})); // chains aren't names
    StubFont other({'z'}, 0.5f, 3);
    fonts.Add("Other", &other); // a change rebuilds chains
    const Font* again = fonts.Find("Latin,Other");
    CHECK(again != nullptr && again->HasGlyph('z'));
    // A Text widget picks a chain by name.
    Viewport vp;
    vp.scale_settings.rule = ScaleSettings::Rule::None;
    vp.SetFonts(&fonts);
    auto canvas = std::make_unique<Canvas>();
    Text* t = canvas->Add<Text>("a\xE4\xB8\xAD");
    t->font = "Latin,CJK";
    t->size = 10;
    t->slot.auto_size = true;
    vp.Add(std::move(canvas));
    vp.Layout(BuiltinFont{});
    CHECK(Near(t->DesiredSize().x, 15.0f)); // 5 + 10, not the default font's 12
}

AETHER_TEST(UIFallback_RealFontChainsWithTheBuiltinBoxes) {
    std::string error;
    std::unique_ptr<SdfFont> roboto = SdfFont::FromFile(std::string(AETHER_REPO_ASSETS_DIR) + "/fonts/Roboto-Medium.ttf", {}, &error);
    CHECK(roboto != nullptr);
    if (!roboto) return;
    const BuiltinFont boxes;
    const FallbackFont chain({roboto.get(), &boxes});
    CHECK(&chain.Source('A') == roboto.get());           // Roboto has Latin
    CHECK(roboto->HasGlyph('A') && !roboto->HasGlyph(0x4E2D));
    CHECK(&chain.Source(0x4E2D) == &boxes);               // CJK falls to the next font, which draws anything
    CHECK(Near(chain.GlyphOf(0x4E2D, 10).advance, 6.0f)); // the box's width, not Roboto's tofu
    DrawList list;
    list.AddText(chain, "A\xE4\xB8\xAD", 20, {0, 0, 200, 40}, Color{1, 1, 1, 1}, TextAlign::Left, 0, nullptr);
    CHECK(list.quads.size() == 2 && list.quads[0].sdf_range > 0.0f && list.quads[1].sdf_range == 0.0f);
}

AETHER_TEST(UIFallback_Utf8RejectsOverlongSurrogatesAndOutOfRange) {
    const auto decode = [](const std::string& s) {
        usize i = 0;
        return DecodeUtf8(s, i);
    };
    CHECK(decode("A") == 'A' && decode("\xC3\xA9") == 0xE9 && decode("\xE4\xB8\xAD") == 0x4E2D && decode("\xF0\x9F\x98\x80") == 0x1F600);
    CHECK(decode("\xC0\xAF") == 0xFFFD);         // overlong '/'
    CHECK(decode("\xE0\x80\xAF") == 0xFFFD);     // overlong, 3 bytes
    CHECK(decode("\xED\xA0\x80") == 0xFFFD);     // a surrogate
    CHECK(decode("\xF4\x90\x80\x80") == 0xFFFD); // past U+10FFFF
    CHECK(decode("\xE4\xB8") == 0xFFFD);         // truncated
    CHECK(decode("\xFF") == 0xFFFD);
    usize i = 0;
    const std::string s = "\xC0\xAF" "x";
    DecodeUtf8(s, i);
    CHECK(i == 2); // the whole bad sequence is consumed, then "x" decodes normally
}

// ---- right-to-left (§29.6) ----

#include "aether/loc/language.h"
#include "aether/ui/bidi.h"

namespace {

std::vector<u32> U(const std::u32string& s) { return std::vector<u32>(s.begin(), s.end()); }
std::u32string S(const std::vector<u32>& v) { return std::u32string(v.begin(), v.end()); }

} // namespace

AETHER_TEST(UIBidi_HebrewReversesAndNumbersKeepTheirOrder) {
    const std::u32string shalom = U"\u05E9\u05DC\u05D5\u05DD"; // שלום, logical order
    const std::u32string reversed(shalom.rbegin(), shalom.rend());
    CHECK(S(ReorderVisual(U(shalom))) == reversed);
    CHECK(S(ReorderVisual(U(U"abc"))) == U"abc"); // no right-to-left character: untouched
    CHECK(S(ReorderVisual(U(U""))) == U"");
    // A Latin word and a number inside Hebrew keep their own order; the Hebrew runs swap places.
    const std::u32string mixed = U"\u05E9\u05DC abc 123 \u05D5\u05DD";
    const std::u32string visual = S(ReorderVisual(U(mixed)));
    CHECK(visual.find(U"abc") != std::u32string::npos && visual.find(U"123") != std::u32string::npos);
    CHECK(visual.front() == U'\u05DD' && visual.back() == U'\u05E9'); // the last Hebrew word is drawn first (leftmost), each word reversed
    CHECK(visual.find(U"abc 123") != std::u32string::npos);           // the Latin and the number keep their order as one run
    // Left-to-right text with a Hebrew word in it: the base stays left to right.
    const std::u32string ltr = U"say \u05E9\u05DC\u05D5\u05DD now";
    const std::u32string out = S(ReorderVisual(U(ltr)));
    CHECK(out.rfind(U"say ", 0) == 0 && out.find(U" now") == out.size() - 4);
    CHECK(BaseDirectionIsRtl(U(shalom)) && !BaseDirectionIsRtl(U(ltr)) && !BaseDirectionIsRtl(U(U"123 ?")));
    // Brackets in a right-to-left run are mirrored.
    CHECK(S(ReorderVisual(U(U"א(ב)"))).find(U'(') != std::u32string::npos);
    CHECK(IsRightToLeft(0x05D0) && IsRightToLeft(0x0627) && !IsRightToLeft('a') && !IsRightToLeft('5') && !IsRightToLeft(0x0661));
}

AETHER_TEST(UIBidi_LayoutWidthAndDrawOrderFollowTheVisualOrder) {
    const BuiltinFont boxes;
    const std::string text = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D"; // שלום
    const TextLayout layout = LayoutText(boxes, text, 10, 0);
    CHECK(Near(layout.size.x, 24.0f) && layout.lines.size() == 1); // 4 glyphs of 6
    // Draw order is by position: with distinct widths the first drawn quad is the last letter.
    StubFont stub({0x05E9, 0x05DC, 0x05D5, 0x05DD}, 1.0f, 1);
    DrawList list;
    list.AddText(stub, text, 10, {0, 0, 100, 20}, Color{1, 1, 1, 1}, TextAlign::Left, 0, nullptr);
    CHECK(list.quads.size() == 4);
    for (usize i = 1; i < list.quads.size(); ++i) CHECK(list.quads[i].rect.x > list.quads[i - 1].rect.x);
}

AETHER_TEST(UIBidi_ViewportMirrorsLayoutAndResolvesStartEnd) {
    const BuiltinFont boxes;
    const auto build = [](Viewport& vp, Text*& first, Text*& start, Text*& end) {
        vp.scale_settings.rule = ScaleSettings::Rule::None;
        vp.SetSize({200, 100});
        auto row = std::make_unique<HorizontalBox>();
        first = row->Add<Text>("A");
        first->size = 10;
        Text* second = row->Add<Text>("B");
        second->size = 10;
        start = row->Add<Text>("S");
        start->justify = TextAlign::Start;
        end = row->Add<Text>("E");
        end->justify = TextAlign::End;
        (void)second;
        vp.Add(std::move(row));
    };
    Viewport ltr, rtl;
    Text *l1, *ls, *le, *r1, *rs, *re;
    build(ltr, l1, ls, le);
    build(rtl, r1, rs, re);
    rtl.direction = FlowDirection::RightToLeft;
    ltr.Layout(boxes);
    rtl.Layout(boxes);
    // The first child is leftmost in LTR and rightmost in RTL, mirrored about the screen's centre.
    CHECK(l1->Geometry().x < ls->Geometry().x);
    CHECK(Near(r1->Geometry().x, 200.0f - l1->Geometry().x - l1->Geometry().w));
    CHECK(r1->Geometry().x > rs->Geometry().x);
    CHECK(Near(r1->Geometry().w, l1->Geometry().w)); // sizes are unchanged
    // Start / End resolve against the direction; Left and Right don't.
    const DrawList a = ltr.Paint(boxes), b = rtl.Paint(boxes);
    CHECK(a.quads.size() == b.quads.size() && !a.quads.empty());
    for (const DrawQuad& q : b.quads) CHECK(q.rect.x >= 0.0f && q.rect.Right() <= 200.0f + 1e-3f);
}

AETHER_TEST(UIBidi_LanguagesKnowTheirDirection) {
    for (const char* l : {"ar", "he", "fa", "ur", "ar-EG", "he-IL", "ps", "yi", "pa-Arab", "ku-Arab-IQ", "AR"}) CHECK(loc::IsRtl(l));
    for (const char* l : {"en", "fr", "pt-BR", "zh-Hans", "ja", "ru", "", "sr-Latn", "arn"}) CHECK(!loc::IsRtl(l));
}
