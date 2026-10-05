#include "test_framework.h"

#include "aether/loc/gather.h"
#include "aether/loc/merge.h"
#include "aether/loc/po.h"
#include "aether/loc/project_sync.h"
#include "aether/reflection/serialize.h"
#include "aether/loc/loc_text.h"

#include <filesystem>
#include <fstream>

// Phase 29 step 4 (§29.4): gathering strings from content, merging keys into
// a table, .po files, and the whole sync the aether_loc tool runs.

using namespace aether;
using namespace aether::loc;
using reflect::Json;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

stdfs::path Dir(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_loc_tools_tests" / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    return dir;
}

void Write(const stdfs::path& file, const std::string& text) {
    stdfs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string Slurp(const stdfs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const GatheredText* Find(const GatherReport& r, const std::string& key) {
    for (const GatheredText& t : r.entries) {
        if (t.key == key) return &t;
    }
    return nullptr;
}

} // namespace

AETHER_TEST(LocGather_FindsWidgetKeysLocTextAndBlueprintLiterals) {
    GatherReport r;
    // A UI layout: a Text and a TextInput, one nested in a child.
    GatherJson(Json::parse(R"({"root":{"type":"VerticalBox","children":[
        {"type":"Text","text":"Play","text_key":"menu.play"},
        {"type":"Border","children":[{"type":"TextInput","hint":"Your name","hint_key":"menu.name"}]},
        {"type":"Text","text":"No key here"}]}})"), "UI/menu.aui", r);
    // A scene: a reflected LocText (with the $v the serializer adds).
    LocText title{"quest.title", "The Lost Cave"};
    Json scene = {{"entities", Json::array({{{"components", {{"QuestLog", {{"title", reflect::ToJson(title)}, {"count", 3}}}}}}})}};
    GatherJson(scene, "Scenes/main.ascene", r);
    // A Blueprint: literal keys, and one from a linked pin.
    GatherJson(Json::parse(R"({"graphs":[{"name":"EventGraph","nodes":[
        {"id":1,"type":"Call.Native:Localize.GetText","defaults":{"key":"hud.score","default":"Score"}},
        {"id":2,"type":"Call.Native:UI.SetTextKey","defaults":{"key":"hud.hp","default":"HP","widget":"Hp"}},
        {"id":3,"type":"Call.Native:Localize.FormatInt","defaults":{"default":"{count} coins"}}]}]})"),
               "Blueprints/hud.abp", r);
    CHECK(r.entries.size() == 5);
    GatherReport sorted;
    for (const GatheredText& t : r.entries) sorted.entries.push_back(t);
    CHECK(Find(sorted, "menu.play") && Find(sorted, "menu.play")->source == "Play");
    CHECK(Find(sorted, "menu.name") && Find(sorted, "menu.name")->source == "Your name");
    CHECK(Find(sorted, "quest.title") && Find(sorted, "quest.title")->source == "The Lost Cave" && Find(sorted, "quest.title")->where.find("Scenes/main.ascene") == 0);
    CHECK(Find(sorted, "hud.score")->source == "Score" && Find(sorted, "hud.hp")->source == "HP");
    CHECK(r.warnings.size() == 1 && r.warnings[0].find("isn't a literal") != std::string::npos); // node 3
    // A struct that merely has key and source fields among others is not a LocText.
    GatherReport other;
    GatherJson(Json::parse(R"({"a":{"key":"x","source":"y","extra":1},"b":{"key":"","source":"z"}})"), "f", other);
    CHECK(other.entries.empty());
}

AETHER_TEST(LocGather_ContentIsMergedByKeyAndConflictsWarn) {
    const stdfs::path content = Dir("content");
    Write(content / "UI/a.aui", R"({"root":{"type":"Text","text":"Play","text_key":"menu.play"}})");
    Write(content / "UI/b.aui", R"({"root":{"type":"Text","text":"Play","text_key":"menu.play"}})");       // same text: fine
    Write(content / "UI/c.aui", R"({"root":{"type":"Text","text":"Start","text_key":"menu.play"}})");      // a different one
    Write(content / "Scenes/s.ascene", R"({"entities":[{"components":{"X":{"t":{"key":"k.only","source":"Only"}}}}]})");
    Write(content / "Broken/bad.ascene", "{ not json");
    Write(content / "notes.txt", R"({"text_key":"ignored","text":"x"})");
    const GatherReport r = GatherContent(content);
    CHECK(r.entries.size() == 2);
    CHECK(r.entries[0].key == "k.only" && r.entries[1].key == "menu.play");
    CHECK(Find(r, "menu.play")->source == "Play"); // the first, by path
    int conflicts = 0, broken = 0;
    for (const std::string& w : r.warnings) {
        conflicts += w.find("two different texts") != std::string::npos;
        broken += w.find("bad.ascene") != std::string::npos;
    }
    CHECK(conflicts == 1 && broken == 1);
    CHECK(GatherContent(content / "missing").entries.empty());
}

AETHER_TEST(LocMerge_AddsKeysKeepsTranslationsReportsUnused) {
    StringTable table;
    table.Set("en", "play", "Play");
    table.Set("fr", "play", "Jouer");
    table.Set("en", "old", "Old");
    table.Set("fr", "only_fr", "Seulement");
    const MergeResult r = MergeKeys(table, "en", {{"play", "Play now"}, {"new", "New thing"}, {"nosrc", ""}});
    CHECK(r.added == 2 && r.updated_source == 1);
    CHECK(*table.Find("en", "play") == "Play now" && *table.Find("fr", "play") == "Jouer"); // the translation is untouched
    CHECK(*table.Find("en", "new") == "New thing" && *table.Find("en", "nosrc") == "nosrc");
    CHECK(table.Find("en", "only_fr") == nullptr); // not gathered: left as it was
    CHECK((r.unused == std::vector<std::string>{"old", "only_fr"}));
    CHECK(table.HasKey("old")); // never removed
    // Nothing changes the second time.
    const MergeResult again = MergeKeys(table, "en", {{"play", "Play now"}, {"new", "New thing"}, {"nosrc", ""}});
    CHECK(again.added == 0 && again.updated_source == 0);
}

AETHER_TEST(LocPo_ExportsAndRoundTrips) {
    StringTable table;
    table.Set("en", "greeting", "Say \"hi\"\nthen\\leave\tnow");
    table.Set("fr", "greeting", "Dites \"salut\"\npuis\\partez\tmaintenant");
    table.Set("en", "play", "Play");
    const std::string po = ExportPo(table, "fr", "en", "Demo");
    CHECK(po.find("msgid \"\"\nmsgstr \"\"") == 0 && po.find("Language: fr") != std::string::npos && po.find("charset=UTF-8") != std::string::npos);
    CHECK(po.find("msgctxt \"greeting\"") != std::string::npos && po.find("msgid \"Say \\\"hi\\\"\\nthen\\\\leave\\tnow\"") != std::string::npos);
    CHECK(po.find("msgctxt \"play\"\nmsgid \"Play\"\nmsgstr \"\"") != std::string::npos); // untranslated: empty
    CHECK(PoLanguage(po) == "fr");
    StringTable back;
    std::vector<std::string> errors;
    CHECK(ImportPo(back, "fr", po, &errors) && errors.empty());
    CHECK(back.KeyCount() == 1 && *back.Find("fr", "greeting") == "Dites \"salut\"\npuis\\partez\tmaintenant"); // play had no translation
}

AETHER_TEST(LocPo_ImportRulesAndErrors) {
    StringTable t;
    std::vector<std::string> errors;
    const std::string po = "# translator comment\n"
                           "msgid \"\"\nmsgstr \"\"\n\"Language: de\\n\"\n\n"
                           "msgctxt \"a\"\nmsgid \"A\"\nmsgstr \"Ah\"\n\n"
                           "#, fuzzy\nmsgctxt \"b\"\nmsgid \"B\"\nmsgstr \"Be\"\n\n"
                           "msgctxt \"c\"\nmsgid \"C\"\nmsgstr \"\"\n\n"
                           "msgctxt \"d\"\nmsgid \"\"\n\"split \"\n\"line\"\nmsgstr \"\"\n\"ge\"\n\"teilt\"\n\n"
                           "#~ msgctxt \"gone\"\n#~ msgid \"G\"\n#~ msgstr \"Weg\"\n\n"
                           "msgid \"No context\"\nmsgstr \"Kein Kontext\"\n";
    CHECK(ImportPo(t, "de", po, &errors));
    CHECK(*t.Find("de", "a") == "Ah");
    CHECK(t.Find("de", "b") == nullptr && errors.size() == 1 && errors[0].find("fuzzy") != std::string::npos);
    CHECK(t.Find("de", "c") == nullptr && t.Find("de", "gone") == nullptr);
    CHECK(*t.Find("de", "d") == "geteilt");
    CHECK(*t.Find("de", "No context") == "Kein Kontext"); // a .po from another tool: the msgid is the key
    CHECK(PoLanguage(po) == "de" && PoLanguage("msgid \"\"\n") == "");
    // Malformed input names its line, and what came before is kept.
    StringTable bad;
    errors.clear();
    CHECK(!ImportPo(bad, "de", "msgctxt \"x\"\nmsgid \"X\"\nmsgstr \"Ix\"\n\nmsgctxt oops\n", &errors));
    CHECK(errors.size() == 1 && errors[0].find("line 5") == 0);
    CHECK(*bad.Find("de", "x") == "Ix");
    errors.clear();
    CHECK(!ImportPo(bad, "de", "msgstr \"bad \\q escape\"\n", &errors));
    CHECK(!ImportPo(bad, "de", "garbage line\n", &errors));
    CHECK(!ImportPo(bad, "de", "\"orphan\"\n", &errors));
}

AETHER_TEST(LocSync_GathersMergesAndWritesFiles) {
    const stdfs::path root = Dir("sync");
    const stdfs::path content = root / "Content";
    Write(content / "UI/menu.aui", R"({"root":{"type":"Text","text":"Play","text_key":"menu.play"}})");
    SyncOptions o;
    o.content_dir = content;
    o.strings_file = content / "Localization/strings.astrings";
    o.po_out = root / "po";
    // The table already has a French translation of a string and a key nothing uses.
    Write(o.strings_file, "key,en,fr\nmenu.play,Play,Jouer\nlegacy,Old,Ancien\n");
    SyncReport r = SyncProject(o);
    CHECK(r.ok && r.gathered.entries.size() == 1 && r.merge.added == 0 && r.merge.unused == std::vector<std::string>{"legacy"});
    CHECK(r.po_written == 1 && stdfs::exists(root / "po/fr.po"));
    CHECK(Slurp(root / "po/fr.po").find("msgstr \"Jouer\"") != std::string::npos);
    // A new string appears in the content; a translator returns a .po.
    Write(content / "UI/hud.aui", R"({"root":{"type":"Text","text":"Score","text_key":"hud.score"}})");
    Write(root / "back/fr.po", "msgid \"\"\nmsgstr \"\"\n\"Language: fr\\n\"\n\nmsgctxt \"hud.score\"\nmsgid \"Score\"\nmsgstr \"Points\"\n");
    o.po_in = root / "back";
    r = SyncProject(o);
    CHECK(r.ok && r.merge.added == 1 && r.po_imported == 1);
    const std::string csv = Slurp(o.strings_file);
    CHECK(csv.find("hud.score,Score,Points") != std::string::npos && csv.find("menu.play,Play,Jouer") != std::string::npos);
    CHECK(csv.find("legacy,Old,Ancien") != std::string::npos); // kept
    // A second run with nothing new rewrites identical files.
    const std::string before = Slurp(o.strings_file), po_before = Slurp(root / "po/fr.po");
    r = SyncProject(o);
    CHECK(r.ok && r.merge.added == 0 && Slurp(o.strings_file) == before && Slurp(root / "po/fr.po") == po_before);
    // --check writes nothing.
    Write(content / "UI/new.aui", R"({"root":{"type":"Text","text":"New","text_key":"new.key"}})");
    o.write = false;
    r = SyncProject(o);
    CHECK(r.ok && r.merge.added == 1 && Slurp(o.strings_file) == before);
    // A damaged table stops it before anything is written.
    o.write = true;
    Write(o.strings_file, "name,en\nx,y\n");
    r = SyncProject(o);
    CHECK(!r.ok && !r.messages.empty());
    SyncOptions empty_lang;
    empty_lang.source_language = " ";
    CHECK(!SyncProject(empty_lang).ok);
}
