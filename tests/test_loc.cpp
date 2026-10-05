#include "test_framework.h"

#include "aether/loc/language.h"
#include "aether/loc/localizer.h"
#include "aether/loc/message_format.h"
#include "aether/loc/string_table.h"
#include "aether/reflection/serialize.h"

// Phase 29 step 1 (§29.1): LocText, string tables and CSV, the fallback
// chain, plural-aware message formatting, and the Localizer.

using namespace aether;
using namespace aether::loc;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

struct Menu {
    LocText title;
    LocText hint;
    i32 width = 0;
};

} // namespace
AETHER_REFLECT(Menu, 1, AETHER_FIELD(title, Field_EditAnywhere), AETHER_FIELD(hint, Field_EditAnywhere), AETHER_FIELD(width, Field_EditAnywhere))

AETHER_TEST(Loc_LanguageTagsNormalizeAndFallBack) {
    CHECK(NormalizeLanguage("pt_br") == "pt-BR");
    CHECK(NormalizeLanguage("PT-br") == "pt-BR");
    CHECK(NormalizeLanguage("zh-hans-cn") == "zh-Hans-CN");
    CHECK(NormalizeLanguage(" EN ") == "en");
    CHECK(NormalizeLanguage("").empty());
    CHECK((FallbackChain("pt-BR") == std::vector<std::string>{"pt-BR", "pt", "en"}));
    CHECK((FallbackChain("zh-Hans-CN") == std::vector<std::string>{"zh-Hans-CN", "zh-Hans", "zh", "en"}));
    CHECK((FallbackChain("en-GB") == std::vector<std::string>{"en-GB", "en"}));
    CHECK((FallbackChain("en") == std::vector<std::string>{"en"})); // no repeat
    CHECK((FallbackChain("") == std::vector<std::string>{"en"}));
    CHECK((FallbackChain("fr-CA", "fr") == std::vector<std::string>{"fr-CA", "fr"}));
}

AETHER_TEST(Loc_StringTableSetFindRemove) {
    StringTable table;
    CHECK(table.Empty());
    table.Set("en", "play", "Play");
    table.Set("pt_BR", "play", "Jogar");
    table.Set("en", "", "ignored");     // no key
    table.Set("", "play", "ignored");    // no language
    CHECK(table.KeyCount() == 1);
    CHECK(*table.Find("en", "play") == "Play");
    CHECK(*table.Find("pt-BR", "play") == "Jogar"); // normalized
    CHECK(table.Find("fr", "play") == nullptr && table.Find("en", "quit") == nullptr);
    CHECK((table.Languages() == std::vector<std::string>{"en", "pt-BR"}));
    CHECK(table.Count("en") == 1 && table.Count("fr") == 0);
    CHECK(table.Remove("pt-BR", "play") && !table.Remove("pt-BR", "play"));
    CHECK((table.Languages() == std::vector<std::string>{"en"}));
    CHECK(table.Remove("en", "play") && table.Empty());
}

AETHER_TEST(Loc_CsvRoundTripsHardCells) {
    StringTable table;
    table.Set("en", "greeting", "Hello, \"world\"");
    table.Set("en", "poem", "Roses\nare red");
    table.Set("pt-BR", "greeting", "Olá, \"mundo\"");
    table.Set("pt-BR", "plain", "Simples");
    const std::string csv = table.ExportCsv();
    CHECK(csv.rfind("key,en,pt-BR\r\n", 0) == 0);
    StringTable back;
    std::vector<std::string> errors;
    CHECK(back.ImportCsv(csv, &errors) && errors.empty());
    CHECK(back.KeyCount() == 3);
    CHECK(*back.Find("en", "greeting") == "Hello, \"world\"");
    CHECK(*back.Find("en", "poem") == "Roses\nare red");
    CHECK(*back.Find("pt-BR", "plain") == "Simples");
    CHECK(back.Find("en", "plain") == nullptr); // an empty cell is no translation
    CHECK(back.ExportCsv() == csv);             // stable
}

AETHER_TEST(Loc_CsvToleratesBomCrlfBlankLinesAndComments) {
    const std::string csv = "\xEF\xBB\xBFkey,comment,en,pt\r\n\r\nbuy,\"Shop button\",Buy,Comprar\r\nsell,,Sell\r\n";
    StringTable table;
    std::vector<std::string> errors;
    CHECK(table.ImportCsv(csv, &errors) && errors.empty());
    CHECK(table.KeyCount() == 2);
    CHECK(*table.Find("en", "buy") == "Buy" && *table.Find("pt", "buy") == "Comprar");
    CHECK(*table.Find("en", "sell") == "Sell" && table.Find("pt", "sell") == nullptr); // a short row
    CHECK((table.Languages() == std::vector<std::string>{"en", "pt"}));               // not "comment"
}

AETHER_TEST(Loc_CsvReportsProblems) {
    StringTable table;
    std::vector<std::string> errors;
    CHECK(!table.ImportCsv("", &errors) && errors.size() == 1);
    errors.clear();
    CHECK(!table.ImportCsv("name,en\nx,y\n", &errors) && errors.size() == 1);
    errors.clear();
    CHECK(!table.ImportCsv("key,en\nx,\"never closes\n", &errors) && errors.size() == 1);
    errors.clear();
    CHECK(table.ImportCsv("key,en\n,orphan\na,one\na,two\n", &errors));
    CHECK(errors.size() == 2); // a row with no key, a repeated key
    CHECK(*table.Find("en", "a") == "two");
    CHECK(table.KeyCount() == 1);
    // Imports merge into what is there.
    CHECK(table.ImportCsv("key,en,fr\nb,bee,abeille\n"));
    CHECK(table.KeyCount() == 2 && *table.Find("fr", "b") == "abeille");
}

AETHER_TEST(Loc_PluralCategories) {
    CHECK(PluralCategory("en", 1) == "one" && PluralCategory("en", 0) == "other" && PluralCategory("en", 2) == "other");
    CHECK(PluralCategory("en", 1.5) == "other");
    CHECK(PluralCategory("pt-BR", 0) == "one" && PluralCategory("pt-BR", 1) == "one" && PluralCategory("pt-BR", 2) == "other");
    CHECK(PluralCategory("pt-PT", 0) == "other" && PluralCategory("pt-PT", 1) == "one");
    CHECK(PluralCategory("fr", 0) == "one" && PluralCategory("fr", 1.5) == "one" && PluralCategory("fr", 2) == "other");
    CHECK(PluralCategory("ja", 1) == "other");
    CHECK(PluralCategory("ru", 1) == "one" && PluralCategory("ru", 21) == "one" && PluralCategory("ru", 11) == "many");
    CHECK(PluralCategory("ru", 3) == "few" && PluralCategory("ru", 13) == "many" && PluralCategory("ru", 5) == "many");
    CHECK(PluralCategory("pl", 1) == "one" && PluralCategory("pl", 22) == "few" && PluralCategory("pl", 12) == "many");
    CHECK(PluralCategory("ar", 0) == "zero" && PluralCategory("ar", 2) == "two" && PluralCategory("ar", 7) == "few");
    CHECK(PluralCategory("ar", 11) == "many" && PluralCategory("ar", 100) == "other");
    CHECK(PluralCategory("xx", 1) == "one"); // unknown follows English
}

AETHER_TEST(Loc_FormatSubstitutesAndPluralizes) {
    FormatArgs one{{"count", i64{1}}, {"name", std::string("Ada")}};
    FormatArgs many{{"count", i64{3}}, {"name", std::string("Ada")}};
    CHECK(Format("{count} coin{count|s}", "en", one) == "1 coin");
    CHECK(Format("{count} coin{count|s}", "en", many) == "3 coins");
    CHECK(Format("{count} coin{count|s}", "en", FormatArgs{{"count", i64{0}}}) == "0 coins");
    CHECK(Format("{name} has {count|one:a coin;other:{coins}}", "en", many) != ""); // doesn't throw
    CHECK(Format("{count|one:coin;other:coins}", "en", one) == "coin");
    CHECK(Format("{count|one:moeda;other:moedas}", "pt-BR", FormatArgs{{"count", i64{0}}}) == "moeda"); // pt-BR: 0 is one
    CHECK(Format("{count|one:файл;few:файла;many:файлов;other:файла}", "ru", FormatArgs{{"count", i64{3}}}) == "файла");
    CHECK(Format("{count|one:file;other:files}", "ar", FormatArgs{{"count", i64{0}}}) == "files"); // no zero form: other
    CHECK(Format("{x} is {y}", "en", FormatArgs{{"x", 2.5}, {"y", std::string("half")}}) == "2.5 is half");
    CHECK(Format("{ name }", "en", one) == "Ada"); // spaces around the name
    CHECK(Format("no braces", "en", {}) == "no braces");
}

AETHER_TEST(Loc_FormatEscapesAndMalformedPatterns) {
    CHECK(Format("{{literal}}", "en", {}) == "{literal}");
    std::vector<std::string> errors;
    CHECK(Format("hi {who}", "en", {}, &errors) == "hi {who}" && errors.size() == 1);
    errors.clear();
    CHECK(Format("open {brace", "en", {}, &errors) == "open {brace" && errors.size() == 1);
    errors.clear();
    CHECK(Format("stray } here", "en", {}, &errors) == "stray } here" && errors.size() == 1);
    errors.clear();
    FormatArgs text{{"s", std::string("abc")}};
    CHECK(Format("{s|s}", "en", text, &errors) == "{s|s}" && errors.size() == 1); // a plural of a string
    CHECK(Format("{a}{", "en", {{"a", i64{1}}}) == "1{");                          // never throws, with no error sink
}

AETHER_TEST(Loc_LocalizerResolvesThroughTheChain) {
    StringTable table;
    table.Set("en", "play", "Play");
    table.Set("pt", "play", "Jogar");
    table.Set("pt-BR", "quit", "Sair");
    table.Set("en", "quit", "Quit");
    table.Set("en", "coins", "{count} coin{count|s}");
    Localizer loc;
    loc.SetTable(&table);
    loc.SetLanguage("pt_BR");
    CHECK(loc.Language() == "pt-BR");
    CHECK(loc.Resolve(LocText{"play", "Play!"}) == "Jogar");   // pt-BR has none: pt does
    CHECK(loc.Resolve(LocText{"quit", "Quit!"}) == "Sair");    // the exact language
    CHECK(loc.Resolve(LocText{"coins", ""}) == "{count} coin{count|s}"); // no args: as stored
    loc.SetLanguage("de");
    CHECK(loc.Resolve(LocText{"play", "Play!"}) == "Play");    // de falls back to the default language
    CHECK(loc.Resolve(LocText{"missing", "Source text"}) == "Source text");
    CHECK(loc.Resolve(LocText{"missing", ""}) == "missing");   // never blank
    CHECK(loc.Resolve(std::string("missing")) == "missing");
    CHECK(loc.Resolve(LocText::Literal("{{as is}}")) == "{{as is}}");
    FormatArgs two{{"count", i64{2}}};
    CHECK(loc.Resolve(LocText{"coins", ""}, &two) == "2 coins");
    CHECK(loc.IsTranslated(LocText{"play", ""}) && !loc.IsTranslated(LocText{"missing", ""}) && !loc.IsTranslated(LocText::Literal("x")));
    // A different default language.
    loc.SetDefaultLanguage("pt");
    CHECK((loc.Chain() == std::vector<std::string>{"de", "pt"}));
    CHECK(loc.Resolve(LocText{"play", ""}) == "Jogar");
    // No table: source, then key.
    Localizer bare;
    CHECK(bare.Resolve(LocText{"k", "src"}) == "src" && bare.Resolve(LocText{"k", ""}) == "k");
}

AETHER_TEST(Loc_LocTextIsAReflectedField) {
    LocText a{"menu.play", "Play"};
    CHECK(!a.IsLiteral() && !a.IsEmpty() && LocText::Literal("x").IsLiteral() && LocText{}.IsEmpty());
    CHECK(a == (LocText{"menu.play", "Play"}) && !(a == LocText::Literal("Play")));
    Menu menu;
    menu.title = {"menu.title", "Main Menu"};
    menu.hint = LocText::Literal("Press any key");
    menu.width = 640;
    const reflect::Json j = reflect::ToJson(menu);
    CHECK(j["title"]["key"] == "menu.title" && j["title"]["source"] == "Main Menu");
    Menu back;
    CHECK(reflect::FromJson(back, j));
    CHECK(back.title == menu.title && back.hint == menu.hint && back.width == 640);
}
