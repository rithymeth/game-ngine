#include "aether/math/math.h"
#include "aether/scene/components.h"
#include "aether/ui/basic.h"
#include "aether/ui/controls.h"
#include "aether/ui/font.h"
#include "aether/ui/panels.h"
#include "aether/ui/theme.h"
#include "aether/ui/world_ui.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

// Phase 18 step 5: distance-field fonts from TrueType (atlas, metrics,
// kerning, fallback, sharp scaling, outlines and soft shadows, font
// libraries), and world-space UI (projected and in-world widgets, their
// placement, drawing, hit testing and input).

namespace uitest5 {
struct Health {
    aether::f32 current = 50.0f;
    aether::f32 max = 100.0f;
};
} // namespace uitest5
AETHER_REFLECT(uitest5::Health, 1, AETHER_FIELD(current), AETHER_FIELD(max))

using namespace aether;
using namespace aether::ui;

namespace {

bool Near(f32 a, f32 b, f32 tol = 1e-3f) { return std::fabs(a - b) <= tol; }
bool Near(const Vec3& a, const Vec3& b, f32 tol = 1e-3f) { return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol); }
const BuiltinFont kFont;

const std::string kFontPath = std::string(AETHER_REPO_ASSETS_DIR) + "/fonts/Roboto-Medium.ttf";

// What the UI shader would draw: each pixel centre's coverage (the most of any quad that `keep` accepts).
std::vector<f32> Rasterize(const DrawList& list, const SdfFont& font, int w, int h, const std::function<bool(const DrawQuad&)>& keep = {}) {
    std::vector<f32> img(static_cast<usize>(w * h), 0.0f);
    for (const DrawQuad& q : list.quads) {
        if (keep && !keep(q)) continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(q.rect.x))), x1 = std::min(w, static_cast<int>(std::ceil(q.rect.Right())));
        const int y0 = std::max(0, static_cast<int>(std::floor(q.rect.y))), y1 = std::min(h, static_cast<int>(std::ceil(q.rect.Bottom())));
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const f32 px = static_cast<f32>(x) + 0.5f, py = static_cast<f32>(y) + 0.5f;
                if (px < q.rect.x || px > q.rect.Right() || py < q.rect.y || py > q.rect.Bottom()) continue;
                const Vec2 uv{q.uv.x + (px - q.rect.x) / q.rect.w * q.uv.w, q.uv.y + (py - q.rect.y) / q.rect.h * q.uv.h};
                f32& out = img[static_cast<usize>(y * w + x)];
                out = std::max(out, SdfCoverage(font.SampleDistance(uv), q) * q.color.a);
            }
        }
    }
    return img;
}

// Along row `y`: pixels partly covered, and the total coverage.
void Row(const std::vector<f32>& img, int w, int y, int& partial, f32& sum) {
    partial = 0;
    sum = 0.0f;
    for (int x = 0; x < w; ++x) {
        const f32 c = img[static_cast<usize>(y * w + x)];
        partial += c > 0.02f && c < 0.98f ? 1 : 0;
        sum += c;
    }
}

} // namespace

AETHER_TEST(UI_SdfFonts) {
    std::string error;
    AETHER_CHECK(SdfFont::FromFile("/no/such/font.ttf", {}, &error) == nullptr && error.find("can't open") != std::string::npos);
    AETHER_CHECK(SdfFont::FromMemory({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}, {}, &error) == nullptr && error.find("not a TrueType") != std::string::npos);
    SdfFontSettings tiny_base;
    tiny_base.base_size = 1.0f;
    AETHER_CHECK(SdfFont::FromFile(kFontPath, tiny_base, &error) == nullptr && error.find("settings") != std::string::npos);

    std::unique_ptr<SdfFont> font = SdfFont::FromFile(kFontPath, {}, &error);
    if (!font) std::printf("    %s\n", error.c_str());
    AETHER_CHECK(font != nullptr);
    AETHER_CHECK(font->FamilyName() == "Roboto Medium");
    // A new atlas is a new texture.
    AETHER_CHECK(font->Resized() && font->AtlasWidth() == 512 && font->AtlasHeight() == 256 && font->AtlasPixels().size() == 512u * 256u);
    font->ClearDirty();
    font->SetTexture(7);
    AETHER_CHECK(font->Texture() == 7 && !font->Dirty());

    // Metrics scale with size: ascent minus descent is the size.
    AETHER_CHECK(font->Ascent(48) > 0.0f && font->LineHeight(48) > font->Ascent(48) && font->LineHeight(48) >= 48.0f);
    AETHER_CHECK(Near(font->LineHeight(24) * 2.0f, font->LineHeight(48)) && Near(font->Ascent(96), font->Ascent(48) * 2.0f));
    AETHER_CHECK(Near(font->SdfRange(48), 255.0f * 6.0f / 128.0f) && Near(font->SdfRange(96), font->SdfRange(48) * 2.0f) && Near(font->SdfEdge(), 128.0f / 255.0f));

    // Glyphs rasterize on first use, into the atlas.
    AETHER_CHECK(font->GlyphCount() == 0);
    const Glyph a48 = font->GlyphOf('A', 48);
    AETHER_CHECK(font->GlyphCount() == 1 && font->Dirty() && !font->Resized());
    const AtlasRect dirty = font->DirtyRect();
    AETHER_CHECK(dirty.w > 12 * 2 && dirty.h > 12 * 2); // the glyph plus its field's padding
    AETHER_CHECK(a48.advance > 20.0f && a48.quad.w > 20.0f && a48.quad.y < -30.0f && a48.quad.Bottom() > 0.0f); // above the baseline, padding below
    AETHER_CHECK(a48.uv.x > 0.0f && a48.uv.Right() < 1.0f && a48.uv.y > 0.0f && a48.uv.Bottom() < 1.0f);
    font->ClearDirty();
    const Glyph a24 = font->GlyphOf('A', 24);
    AETHER_CHECK(font->GlyphCount() == 1 && !font->Dirty()); // cached, any size
    AETHER_CHECK(Near(a24.advance * 2.0f, a48.advance) && Near(a24.quad.w * 2.0f, a48.quad.w) && a24.uv.x == a48.uv.x);
    const Glyph space = font->GlyphOf(' ', 48);
    AETHER_CHECK(space.advance > 5.0f && space.quad.Empty() && font->GlyphOf('\t', 48).quad.Empty() && Near(font->GlyphOf('\t', 48).advance, space.advance));
    // Inside the glyph's field is above the edge; its corner (padding) is below.
    const Glyph l = font->GlyphOf('l', 48);
    AETHER_CHECK(font->SampleDistance({l.uv.x + l.uv.w * 0.5f, l.uv.y + l.uv.h * 0.5f}) > font->SdfEdge() + 0.1f);
    AETHER_CHECK(font->SampleDistance({l.uv.x + 0.5f / 512.0f, l.uv.y + 0.5f / 256.0f}) < font->SdfEdge() - 0.2f);
    // Kerning pulls "AV" together; code points the font lacks fall back.
    AETHER_CHECK(font->Kerning('A', 'V', 48) < -0.5f && Near(font->Kerning('A', 'V', 24) * 2.0f, font->Kerning('A', 'V', 48)));
    AETHER_CHECK(font->HasGlyph('A') && !font->HasGlyph(0x4E2D));
    const Glyph missing = font->GlyphOf(0x4E2D, 48);
    const Glyph fallback = font->GlyphOf(font->HasGlyph(0xFFFD) ? 0xFFFD : '?', 48);
    AETHER_CHECK(missing.advance == fallback.advance && missing.uv.x == fallback.uv.x && missing.uv.y == fallback.uv.y);
    // Laid out with its own metrics.
    const TextLayout av = LayoutText(*font, "AV", 48);
    AETHER_CHECK(Near(av.size.x, a48.advance + font->GlyphOf('V', 48).advance + font->Kerning('A', 'V', 48)) && Near(av.size.y, font->LineHeight(48)));

    // The atlas grows when it fills: glyphs keep their pixels, their v changes.
    SdfFontSettings small;
    small.atlas_width = 128;
    small.atlas_height = 32;
    small.max_atlas_height = 512;
    std::unique_ptr<SdfFont> growing = SdfFont::FromFile(kFontPath, small);
    AETHER_CHECK(growing != nullptr);
    const Glyph first = growing->GlyphOf('M', 48);
    const f32 before = growing->SampleDistance({first.uv.x + first.uv.w * 0.5f, first.uv.y + first.uv.h * 0.3f});
    growing->ClearDirty();
    growing->Prewarm("ABCDEFGHIJKLNOP");
    AETHER_CHECK(growing->Resized() && growing->AtlasHeight() > 32 && growing->Problems().empty() && growing->GlyphCount() == 16);
    const Glyph moved = growing->GlyphOf('M', 48);
    AETHER_CHECK(moved.uv.h < first.uv.h && Near(growing->SampleDistance({moved.uv.x + moved.uv.w * 0.5f, moved.uv.y + moved.uv.h * 0.3f}), before, 0.01f));
    // One that can't grow enough reports what didn't fit.
    small.max_atlas_height = 64;
    std::unique_ptr<SdfFont> full = SdfFont::FromFile(kFontPath, small);
    full->Prewarm("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    AETHER_CHECK(!full->Problems().empty() && full->Problems()[0].find("doesn't fit") != std::string::npos && full->AtlasHeight() == 64);
    AETHER_CHECK(full->GlyphOf('Z', 48).advance > 0.0f); // still takes its room

    // --- Drawing ----------------------------------------------------------------------------
    {
        DrawQuad q;
        q.sdf_range = 10.0f;
        AETHER_CHECK(Near(SdfCoverage(q.sdf_edge, q), 0.5f) && SdfCoverage(q.sdf_edge + 0.1f, q) == 1.0f && SdfCoverage(q.sdf_edge - 0.1f, q) == 0.0f);
        q.sdf_softness = 0.2f;
        AETHER_CHECK(SdfCoverage(q.sdf_edge + 0.1f, q) < 0.9f && SdfCoverage(q.sdf_edge + 0.1f, q) > 0.6f);
        q.sdf_range = 0.0f;
        AETHER_CHECK(SdfCoverage(0.0f, q) == 1.0f); // not a field
    }
    DrawList text;
    text.AddText(*font, "Hi", 32, {10, 10, 200, 50}, {});
    AETHER_CHECK(text.quads.size() == 2 && text.quads[0].texture == 7 && Near(text.quads[0].sdf_range, font->SdfRange(32)) && text.quads[0].sdf_edge == font->SdfEdge());
    // The field's span grows with a render transform and with the viewport scale.
    DrawList scaled;
    scaled.PushTransform({}, {2, 2}, {});
    scaled.AddText(*font, "Hi", 32, {10, 10, 200, 50}, {});
    AETHER_CHECK(Near(scaled.quads[0].sdf_range, font->SdfRange(32) * 2.0f));
    scaled.Scale(1.5f);
    AETHER_CHECK(Near(scaled.quads[0].sdf_range, font->SdfRange(32) * 3.0f));

    // Sharp at any size: an 'I' 16 or 200 pixels tall has edges at most a
    // pixel or so wide, and its stem is 12.5 times as wide at 200.
    auto stem = [&](f32 size, int& partial, f32& sum) {
        DrawList list;
        list.AddText(*font, "I", size, {4, 4, 400, 400}, {});
        const int w = 300, h = 300;
        const std::vector<f32> img = Rasterize(list, *font, w, h);
        const Rect& r = list.quads[0].rect;
        Row(img, w, static_cast<int>(r.y + r.h * 0.5f), partial, sum);
    };
    int partial16 = 0, partial200 = 0;
    f32 sum16 = 0.0f, sum200 = 0.0f;
    stem(16, partial16, sum16);
    stem(200, partial200, sum200);
    AETHER_CHECK(partial200 <= 4 && partial16 <= 4 && sum16 > 1.0f);
    AETHER_CHECK(sum200 / sum16 > 12.5f * 0.85f && sum200 / sum16 < 12.5f * 1.15f);

    // Effects: a shadow pass, an outline pass, then the fill.
    TextEffects fx;
    fx.outline = 3.0f;
    fx.outline_color = {0, 0, 0, 1};
    fx.shadow_offset = {4, 4};
    fx.shadow_color = {0, 0, 0, 0.5f};
    fx.shadow_softness = 6.0f;
    AETHER_CHECK(fx.Any() && !TextEffects{}.Any());
    DrawList fancy;
    fancy.AddText(*font, "Hi", 100, {10, 10, 400, 200}, {1, 1, 1, 0.8f}, TextAlign::Left, 0.0f, &fx);
    AETHER_CHECK(fancy.quads.size() == 6);
    const DrawQuad &shadow = fancy.quads[0], &outline = fancy.quads[2], &fill = fancy.quads[4];
    AETHER_CHECK(Near(shadow.rect.x, fill.rect.x + 4) && Near(shadow.rect.y, fill.rect.y + 4) && Near(shadow.color.a, 0.4f) && shadow.sdf_softness > 0.0f);
    AETHER_CHECK(Near(outline.rect.x, fill.rect.x) && outline.sdf_edge < fill.sdf_edge && Near(outline.sdf_edge, fill.sdf_edge - 3.0f / font->SdfRange(100)));
    AETHER_CHECK(Near(outline.color.a, 0.8f) && fill.sdf_softness == 0.0f && outline.sdf_softness == 0.0f);
    {
        // Just outside the fill's edge the outline covers it; the soft shadow fades over many pixels.
        const int w = 400, h = 200;
        const std::vector<f32> fill_img = Rasterize(fancy, *font, w, h, [](const DrawQuad& q) { return q.color.r == 1.0f; });
        const std::vector<f32> outline_img = Rasterize(fancy, *font, w, h, [&](const DrawQuad& q) { return q.color.r == 0.0f && q.sdf_softness == 0.0f; });
        const std::vector<f32> shadow_img = Rasterize(fancy, *font, w, h, [](const DrawQuad& q) { return q.sdf_softness > 0.0f; });
        const int y = static_cast<int>(fill.rect.y + fill.rect.h * 0.5f);
        int left = -1; // the fill's left edge on this row
        for (int x = 0; x < w && left < 0; ++x) left = fill_img[static_cast<usize>(y * w + x)] > 0.5f ? x : -1;
        AETHER_CHECK(left > 0);
        const usize outside = static_cast<usize>(y * w + left - 2);
        AETHER_CHECK(fill_img[outside] < 0.05f && outline_img[outside] > 0.8f * 0.95f); // (the text is 80% opaque)
        int partial = 0;
        f32 sum = 0.0f;
        Row(shadow_img, w, y + 4, partial, sum);
        AETHER_CHECK(partial > 10);
    }
    // A bitmap font gets offset copies for its outline.
    DrawList plain;
    TextEffects outline_only;
    outline_only.outline = 1.0f;
    plain.AddText(kFont, "ab", 20, {0, 0, 100, 30}, {}, TextAlign::Left, 0.0f, &outline_only);
    AETHER_CHECK(plain.quads.size() == 2 * 4 + 2 && plain.quads[0].sdf_range == 0.0f && Near(plain.quads[0].rect.x, plain.quads.back().rect.x - 0.6f * 20 - 1.0f));

    // --- Font libraries, Text widgets, layouts and themes -------------------------------------
    FontLibrary fonts;
    fonts.Add("Roboto", font.get());
    AETHER_CHECK(fonts.Find("Roboto") == font.get() && fonts.Find("Nope") == nullptr && fonts.Names() == std::vector<std::string>{"Roboto"});
    Viewport vp;
    vp.scale_settings.rule = ScaleSettings::Rule::None;
    vp.SetFonts(&fonts);
    auto canvas = std::make_unique<Canvas>();
    Text* named = canvas->Add<Text>("AV");
    named->font = "Roboto";
    named->size = 48;
    named->slot.auto_size = true;
    Text* unnamed = canvas->Add<Text>("AV");
    unnamed->size = 48;
    unnamed->slot.auto_size = true;
    Text* unknown = canvas->Add<Text>("AV");
    unknown->font = "Nope";
    unknown->size = 48;
    unknown->slot.auto_size = true;
    vp.Add(std::move(canvas));
    vp.Layout(kFont);
    AETHER_CHECK(Near(named->DesiredSize().x, av.size.x) && Near(unnamed->DesiredSize().x, 2 * 0.6f * 48) && Near(unknown->DesiredSize().x, 2 * 0.6f * 48));
    const DrawList painted = vp.Paint(kFont);
    AETHER_CHECK(painted.quads.size() == 6 && painted.quads[0].texture == 7 && painted.quads[0].sdf_range > 0.0f && painted.quads[2].sdf_range == 0.0f);
    fonts.Remove("Roboto");
    AETHER_CHECK(fonts.Find("Roboto") == nullptr);

    // Saved in layouts and themes.
    Text saved("Title");
    saved.font = "Roboto";
    saved.effects = fx;
    const std::string layout = SaveLayout(saved);
    AETHER_CHECK(layout.find("\"font\": \"Roboto\"") != std::string::npos && layout.find("shadow_softness") != std::string::npos);
    LayoutDocument doc;
    AETHER_CHECK(LoadLayout(layout, doc) && SaveLayout(*doc.root) == layout);
    auto* loaded = static_cast<Text*>(doc.root.get());
    AETHER_CHECK(loaded->font == "Roboto" && loaded->effects == fx);
    AETHER_CHECK(SaveLayout(Text("x")).find("effects") == std::string::npos); // only when set
    AETHER_CHECK(!LoadLayout(R"({"root":{"type":"Text","effects":{"outline":-1}}})", doc, {}, &error) && error.find("negative") != std::string::npos);
    AETHER_CHECK(!LoadLayout(R"({"root":{"type":"Text","effects":{"shadow_offset":[1]}}})", doc, {}, &error) && error.find("shadow_offset") != std::string::npos);

    Theme theme;
    AETHER_CHECK(LoadTheme(R"({"version":1,"colors":{"ink":"#102030"},
        "text":{"Title":{"size":40,"font":"Roboto","effects":{"outline":2,"outline_color":"@ink","shadow_offset":[2,3],"shadow_color":[0,0,0,0.5]}}}})",
                           theme, {}, &error));
    const TextStyle* title = theme.TextFor("Title");
    AETHER_CHECK(title != nullptr && title->font == "Roboto" && title->effects.outline == 2.0f && Near(title->effects.outline_color.b, 0x30 / 255.0f));
    AETHER_CHECK((title->effects.shadow_offset == Vec2{2, 3}) && title->effects.shadow_softness == 0.0f);
    Text themed("Hello");
    themed.style_class = "Title";
    ApplyTheme(theme, themed);
    AETHER_CHECK(themed.font == "Roboto" && themed.size == 40.0f && themed.effects == title->effects);
    Theme again;
    AETHER_CHECK(LoadTheme(SaveTheme(theme), again) && again.TextFor("Title")->effects == title->effects && again.TextFor("Title")->font == "Roboto");
}

AETHER_TEST(UI_WorldSpaceWidgets) {
    const std::map<std::string, std::string> files = {
        {"HealthBar", R"({"root":{"type":"Canvas","children":[
            {"type":"Text","name":"Name","text":"Enemy","size":16,"slot":{"position":[0,0],"auto_size":true}},
            {"type":"ProgressBar","name":"HP","slot":{"position":[0,20],"size":[140,20]}},
            {"type":"Button","name":"Talk","slot":{"position":[150,5],"size":[40,30]}}]},
            "bindings":[{"widget":"HP","property":"percent","source":"Health.current","divide_by":"Health.max"}]})"},
        {"Console", R"({"root":{"type":"Canvas","children":[{"type":"Button","name":"Use","slot":{"position":[50,25],"size":[100,50]}}]}})"},
    };
    UISystem::LayoutLoader loader = [&](const std::string& name, LayoutDocument& out, std::string* error) {
        const auto it = files.find(name);
        if (it == files.end()) {
            if (error != nullptr) *error = "no such file";
            return false;
        }
        return LoadLayout(it->second, out, {}, error);
    };
    World world;
    (void)GetComponentId<uitest5::Health>();
    WorldUISystem sys(world, kFont, loader);
    AETHER_CHECK(WorldUISystem::Active() == &sys);

    WorldCamera cam;
    cam.view = Mat4::LookAtRH({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    cam.projection = Mat4::PerspectiveRH(Radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    cam.screen = {1920, 1080};
    auto at = [](Vec3 p) {
        Transform t;
        t.position = p;
        return t;
    };

    // A health bar over an enemy ten metres ahead: its bottom centre on the point two metres above it.
    WorldWidgetComponent bar;
    bar.layout = "HealthBar";
    bar.interactive = true;
    const Entity enemy = world.CreateEntity(at({0, 0, -10}), uitest5::Health{}, bar);
    sys.Update(cam, 0.016f);
    for (const std::string& p : sys.Problems()) std::printf("    %s\n", p.c_str());
    AETHER_CHECK(sys.Problems().empty() && sys.RootOf(enemy) != nullptr && sys.ShownCount() == 1);
    const WorldUISystem::Placement* p = sys.PlacementOf(enemy);
    const f32 anchor_y = (0.5f - 0.5f * 0.2f / std::tan(Radians(30.0f))) * 1080.0f;
    AETHER_CHECK(p->shown && Near(p->anchor.x, 960, 0.01f) && Near(p->anchor.y, anchor_y, 0.01f) && p->scale == 1.0f);
    AETHER_CHECK(p->rect.x == 860.0f && p->rect.y == std::round(anchor_y - 40) && p->rect.w == 200.0f && p->rect.h == 40.0f); // whole pixels
    AETHER_CHECK(Near(p->distance, std::sqrt(104.0f)) && p->depth > 0.0f && p->depth < 1.0f);
    // Bound to its own entity's components.
    auto* hp = static_cast<ProgressBar*>(sys.FindWidget(enemy, "HP"));
    AETHER_CHECK(Near(hp->percent, 0.5f));
    world.GetComponent<uitest5::Health>(enemy)->current = 25;
    sys.Update(cam, 0.016f);
    AETHER_CHECK(Near(hp->percent, 0.25f));
    // Drawn in pixels over the entity, clipped to its box.
    DrawList screen = sys.PaintScreen();
    bool found_fill = false;
    for (const DrawQuad& q : screen.quads) {
        AETHER_CHECK(q.rect.x >= 860.0f && q.rect.Right() <= 1060.0f + 1e-3f);
        found_fill |= Near(q.rect.x, 860) && Near(q.rect.y, p->rect.y + 20) && Near(q.rect.w, 35) && Near(q.rect.h, 20);
    }
    AETHER_CHECK(found_fill && sys.PaintWorld().empty());
    AETHER_CHECK(Near(screen.clips[screen.quads[0].clip].x, 860) && Near(screen.clips[screen.quads[0].clip].w, 200));

    // The UI library reaches world widgets too.
    UI::SetText(enemy, "Name", "Goblin");
    AETHER_CHECK(static_cast<Text*>(sys.FindWidget(enemy, "Name"))->text == "Goblin");

    // Clicking its button: the press goes to it, and the click comes back as an event.
    const Vec2 talk{p->rect.x + 170, p->rect.y + 20};
    WorldUISystem::Hit hit = sys.HitTest(talk);
    AETHER_CHECK(hit.entity == enemy && hit.widget == sys.FindWidget(enemy, "Talk") && Near(hit.local.x, 170) && Near(hit.local.y, 20));
    AETHER_CHECK(sys.HitTest({p->rect.x + 50, p->rect.y + 30}).widget == nullptr); // the bar lets clicks through to the game
    AETHER_CHECK(sys.PointerMove(talk) && sys.FindWidget(enemy, "Talk")->IsHovered());
    AETHER_CHECK(sys.PointerDown(talk) && sys.PointerUp(talk));
    sys.Update(cam, 0.016f);
    AETHER_CHECK(sys.Events().size() == 1 && sys.Events()[0].entity == enemy && sys.Events()[0].kind == WidgetEvent::Kind::Clicked && sys.Events()[0].widget == "Talk");
    // Pressed, dragged off and released elsewhere: no click; the pointer leaving unhovers it.
    AETHER_CHECK(sys.PointerDown(talk) && sys.PointerMove({10, 10}) && sys.PointerUp({10, 10}));
    AETHER_CHECK(!sys.PointerMove({10, 10}) && !sys.FindWidget(enemy, "Talk")->IsHovered());
    sys.Update(cam, 0.016f);
    AETHER_CHECK(sys.Events().empty() && !sys.PointerDown({10, 10}));
    // Not interactive: it's still hit-testable, but takes no input.
    world.GetComponent<WorldWidgetComponent>(enemy)->interactive = false;
    sys.Update(cam, 0.016f);
    AETHER_CHECK(!sys.PointerDown(talk) && sys.HitTest(talk).entity == enemy);

    // Farther ones shrink past their reference distance, and draw first.
    WorldWidgetComponent far_bar = bar;
    far_bar.reference_distance = 10.0f;
    far_bar.min_scale = 0.25f;
    const Entity far = world.CreateEntity(at({0, 0, -20}), uitest5::Health{}, far_bar);
    sys.Update(cam, 0.016f);
    const WorldUISystem::Placement* fp = sys.PlacementOf(far);
    AETHER_CHECK(fp->shown && Near(fp->scale, 10.0f / std::sqrt(404.0f)) && Near(fp->rect.w, 200 * fp->scale));
    screen = sys.PaintScreen();
    AETHER_CHECK(screen.quads[0].rect.x >= fp->rect.x && screen.quads[0].rect.Right() <= fp->rect.Right() + 1e-3f && screen.quads[0].rect.w < 100);
    world.GetComponent<Transform>(far)->position = {0, 0, -60};
    sys.Update(cam, 0.016f);
    AETHER_CHECK(Near(sys.PlacementOf(far)->scale, 0.25f)); // no smaller than min_scale
    // Hidden: past max_distance, behind the camera, off screen, or not visible.
    world.GetComponent<WorldWidgetComponent>(far)->max_distance = 50.0f;
    sys.Update(cam, 0.016f);
    AETHER_CHECK(!sys.PlacementOf(far)->shown && sys.ShownCount() == 1);
    world.GetComponent<Transform>(enemy)->position = {0, 0, 10};
    sys.Update(cam, 0.016f);
    AETHER_CHECK(!sys.PlacementOf(enemy)->shown && sys.HitTest(talk).entity == kNullEntity);
    world.GetComponent<Transform>(enemy)->position = {100, 0, -10};
    sys.Update(cam, 0.016f);
    AETHER_CHECK(!sys.PlacementOf(enemy)->shown);
    world.GetComponent<Transform>(enemy)->position = {0, 0, -10};
    world.GetComponent<WorldWidgetComponent>(enemy)->visible = false;
    sys.Update(cam, 0.016f);
    AETHER_CHECK(!sys.PlacementOf(enemy)->shown && sys.PaintScreen().quads.empty());
    world.GetComponent<WorldWidgetComponent>(enemy)->visible = true;
    // A UI scale (the HUD's) scales them all.
    cam.ui_scale = 2.0f;
    sys.Update(cam, 0.016f);
    AETHER_CHECK(Near(sys.PlacementOf(enemy)->rect.w, 400) && Near(sys.PlacementOf(enemy)->rect.x, 760));
    cam.ui_scale = 1.0f;

    // In the world: a screen on a console, facing the camera, two metres wide.
    WorldWidgetComponent console;
    console.layout = "Console";
    console.space = WorldWidgetSpace::World;
    console.offset = {0, 0, 0};
    console.width = 200;
    console.height = 100;
    console.pivot_y = 0.5f;
    console.world_width = 2.0f;
    console.interactive = true;
    const Entity terminal = world.CreateEntity(at({3, 0, -10}), console);
    sys.Update(cam, 0.016f);
    const WorldUISystem::Placement* tp = sys.PlacementOf(terminal);
    AETHER_CHECK(tp->shown && Near(tp->corners[0], {2, 0.5f, -10}) && Near(tp->corners[1], {4, 0.5f, -10}) && Near(tp->corners[2], {4, -0.5f, -10}));
    const Vec4 corner = tp->transform * Vec4(200, 100, 0, 1);
    AETHER_CHECK(Near(Vec3(corner.x, corner.y, corner.z), tp->corners[2]));
    const std::vector<WorldUISystem::WorldDraw> drawn = sys.PaintWorld();
    AETHER_CHECK(drawn.size() == 1 && drawn[0].entity == terminal && (drawn[0].size == Vec2{200, 100}) && !drawn[0].list.quads.empty());
    // Pointing at its middle hits its button at the middle of the layout.
    const f32 px = (0.5f + 0.5f * 0.3f / (16.0f / 9.0f * std::tan(Radians(30.0f)))) * 1920.0f;
    hit = sys.HitTest({px, 540});
    AETHER_CHECK(hit.entity == terminal && hit.widget == sys.FindWidget(terminal, "Use") && Near(hit.local.x, 100, 0.05f) && Near(hit.local.y, 50, 0.05f));
    AETHER_CHECK(Near(hit.distance, std::sqrt(109.0f), 0.01f));
    AETHER_CHECK(sys.PointerMove({px, 540}) && sys.PointerDown({px, 540}) && sys.PointerUp({px, 540}));
    sys.Update(cam, 0.016f);
    AETHER_CHECK(sys.Events().size() == 1 && sys.Events()[0].entity == terminal && sys.Events()[0].widget == "Use");
    // Seen from the side, a billboard turns to face the camera; a fixed one keeps the entity's facing.
    WorldCamera side = cam;
    side.view = Mat4::LookAtRH({13, 0, -10}, {3, 0, -10}, {0, 1, 0});
    sys.Update(side, 0.016f);
    AETHER_CHECK(Near(tp->corners[1] - tp->corners[0], {0, 0, -2}));
    world.GetComponent<WorldWidgetComponent>(terminal)->billboard = false;
    world.GetComponent<Transform>(terminal)->rotation = Quaternion::FromAxisAngle({0, 1, 0}, Radians(90.0f));
    sys.Update(cam, 0.016f);
    AETHER_CHECK(Near(tp->corners[1] - tp->corners[0], {0, 0, -2}));

    // Missing layouts and transforms are reported once; removing the component removes the widget.
    WorldWidgetComponent broken;
    broken.layout = "Nope";
    (void)world.CreateEntity(at({0, 0, -5}), broken);
    WorldWidgetComponent floating = console;
    const Entity nowhere = world.CreateEntity(floating);
    sys.Update(cam, 0.016f);
    sys.Update(cam, 0.016f);
    const std::vector<std::string>& problems = sys.Problems();
    AETHER_CHECK(problems.size() == 2 && std::count_if(problems.begin(), problems.end(), [](const std::string& s) { return s.find("'Nope': no such file") != std::string::npos; }) == 1 &&
                 std::count_if(problems.begin(), problems.end(), [](const std::string& s) { return s.find("no Transform") != std::string::npos; }) == 1);
    AETHER_CHECK(!sys.PlacementOf(nowhere)->shown);
    world.RemoveComponent<WorldWidgetComponent>(enemy);
    sys.Update(cam, 0.016f);
    AETHER_CHECK(sys.RootOf(enemy) == nullptr && sys.PlacementOf(enemy) == nullptr);
    UI::SetText(enemy, "Name", "Gone"); // nothing to change; no crash
}
