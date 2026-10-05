#include "aether/loc/localization.h"
#include "aether/ui/basic.h"
#include "aether/ui/controls.h"
#include "aether/ui/layout_file.h"
#include "aether/ui/panels.h"
#include "aether/ui/theme.h"
#include "aether/ui/viewport.h"
#include "test_framework.h"

#include <cmath>

// Phase 29 step 3 (§29.3): widget text with a string table key.

using namespace aether;
using namespace aether::ui;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

const BuiltinFont kFont;

// A Localization made active for the test, and cleared after.
struct Active {
    loc::Localization l;
    Active() {
        l.AddFromCsv("key,en,fr\nplay,Play,Jouer\nquit,Quit,\nhint,Type here,Tapez ici\n");
        l.MakeActive();
    }
};

} // namespace

AETHER_TEST(UILoc_TextShowsTheTranslationAndFallsBack) {
    Text t("Fallback");
    CHECK(t.Shown() == "Fallback");           // no key
    t.text_key = "play";
    CHECK(t.Shown() == "Fallback");           // no Localization: the source text
    Active a;
    CHECK(t.Shown() == "Play");
    a.l.SetLanguage("fr");
    CHECK(t.Shown() == "Jouer");
    t.text_key = "quit";                      // fr has none: the default language
    CHECK(t.Shown() == "Quit");
    t.text_key = "missing";
    CHECK(t.Shown() == "Fallback");           // not in any table: the source text
    Text plain("Literal");
    CHECK(plain.Shown() == "Literal");        // no key: never looked up
    TextInput in;
    in.hint = "Hint source";
    in.hint_key = "hint";
    CHECK(in.ShownHint() == "Tapez ici");
    in.hint_key.clear();
    CHECK(in.ShownHint() == "Hint source");
}

AETHER_TEST(UILoc_LayoutFollowsTheLanguage) {
    Active a;
    Viewport vp;
    vp.scale_settings.rule = ScaleSettings::Rule::None;
    auto canvas = std::make_unique<Canvas>();
    Text* label = canvas->Add<Text>("Play");
    label->text_key = "play";
    label->size = 10;
    label->slot.auto_size = true;
    vp.Add(std::move(canvas));
    vp.Layout(kFont);
    CHECK(std::fabs(label->DesiredSize().x - 4 * 0.6f * 10) < 1e-3f); // "Play"
    a.l.SetLanguage("fr");
    vp.Layout(kFont);
    CHECK(std::fabs(label->DesiredSize().x - 5 * 0.6f * 10) < 1e-3f); // "Jouer"
    const DrawList painted = vp.Paint(kFont);
    CHECK(painted.quads.size() == 5); // a quad per glyph of "Jouer"
}

AETHER_TEST(UILoc_LayoutJsonKeepsTheKeysAndOldFilesLoad) {
    Text t("Play");
    t.text_key = "play";
    const std::string saved = SaveLayout(t);
    CHECK(saved.find("\"text_key\": \"play\"") != std::string::npos);
    LayoutDocument doc;
    CHECK(LoadLayout(saved, doc) && SaveLayout(*doc.root) == saved);
    CHECK(static_cast<Text*>(doc.root.get())->text_key == "play");
    CHECK(SaveLayout(Text("x")).find("text_key") == std::string::npos); // only when set
    // A file from before the key existed.
    LayoutDocument old;
    CHECK(LoadLayout(R"({"root":{"type":"Text","text":"Old"}})", old));
    const auto* loaded = static_cast<Text*>(old.root.get());
    CHECK(loaded->text == "Old" && loaded->text_key.empty() && loaded->Shown() == "Old");
    TextInput in;
    in.hint = "Type here";
    in.hint_key = "hint";
    LayoutDocument doc2;
    const std::string saved_input = SaveLayout(in);
    CHECK(saved_input.find("hint_key") != std::string::npos && LoadLayout(saved_input, doc2));
    CHECK(static_cast<TextInput*>(doc2.root.get())->hint_key == "hint");
    CHECK(SaveLayout(TextInput{}).find("hint_key") == std::string::npos);
}
