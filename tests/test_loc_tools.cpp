#include "test_framework.h"

#include "aether/loc/gather.h"
#include "aether/loc/merge.h"
#include "aether/loc/po.h"
#include "aether/loc/project_sync.h"
#include "aether/reflection/serialize.h"
#include "aether/loc/loc_text.h"

#include <chrono>
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

AETHER_TEST(LocGather_BlueprintLinksOverrideSavedLocalizationPinDefaults) {
    const stdfs::path content = Dir("blueprint_wired");
    Write(content / "wired.abp", R"({"graphs":[
        {"name":"First","nodes":[
            {"id":1,"type":"Variable.Get"},
            {"id":2,"type":"Call.Native:Localize.GetText","defaults":{"key":"stale.key","default":"Stale"}},
            {"id":3,"type":"Call.Native:Localize.GetText","defaults":{"key":"menu.play","default":"Old source"}}],
         "links":[{"from":[1,"value"],"to":[2,"key"]},{"from":[1,"value"],"to":[3,"default"]}]},
        {"name":"Second","nodes":[
            {"id":2,"type":"Call.Native:Localize.GetText","defaults":{"key":"menu.quit","default":"Quit"}}],
         "links":[]}]})");
    const GatherReport report = GatherContent(content);
    CHECK(!Find(report, "stale.key"));
    const GatheredText* play = Find(report, "menu.play");
    const GatheredText* quit = Find(report, "menu.quit");
    CHECK(play && play->source.empty());
    CHECK(quit && quit->source == "Quit"); // The duplicate node ID belongs to a different graph.
    CHECK(report.warnings.size() == 1 && report.warnings[0].find("key isn't a literal") != std::string::npos);

    SyncOptions options;
    options.content_dir = content;
    options.strings_file = content / "Localization/strings.astrings";
    Write(options.strings_file, "key,en,fr\nmenu.play,Play,Jouer\n");
    CHECK(SyncProject(options).ok);
    const std::string saved = Slurp(options.strings_file);
    CHECK(saved.find("menu.play,Play,Jouer") != std::string::npos);
    CHECK(saved.find("stale.key") == std::string::npos);
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

AETHER_TEST(LocGather_ConflictNamesTheFileThatSuppliedTheSourceText) {
    const stdfs::path content = Dir("source_provenance");
    Write(content / "a-placeholder.aui", R"({"root":{"text_key":"menu.play"}})");
    Write(content / "b-first.aui", R"({"root":{"text_key":"menu.play","text":"Play"}})");
    Write(content / "c-conflict.aui", R"({"root":{"text_key":"menu.play","text":"Start"}})");
    const GatherReport report = GatherContent(content);
    const GatheredText* found = Find(report, "menu.play");
    CHECK(found && found->source == "Play" && found->where == "b-first.aui: /root");
    CHECK(report.warnings.size() == 1);
    if (report.warnings.size() == 1) {
        CHECK(report.warnings[0].find("b-first.aui: /root") != std::string::npos);
        CHECK(report.warnings[0].find("c-conflict.aui: /root") != std::string::npos);
        CHECK(report.warnings[0].find("a-placeholder.aui") == std::string::npos);
    }
}

AETHER_TEST(LocGather_RecognizesAssetExtensionsRegardlessOfCase) {
    const stdfs::path content = Dir("extension_case");
    Write(content / "menu.AUI", R"({"text_key":"ui.play","text":"Play"})");
    Write(content / "level.ASCENE", R"({"label":{"key":"scene.title","source":"Level"}})");
    Write(content / "item.ApReFaB", R"({"label":{"key":"prefab.title","source":"Item"}})");
    Write(content / "logic.aBp", R"({"label":{"key":"blueprint.title","source":"Logic"}})");
    Write(content / "index.TXT", R"({"text_key":"ignored","text":"Notes"})");
    const GatherReport report = GatherContent(content);
    CHECK(report.entries.size() == 4 && report.warnings.empty());
    CHECK(Find(report, "ui.play") && Find(report, "scene.title"));
    CHECK(Find(report, "prefab.title") && Find(report, "blueprint.title"));
    CHECK(!Find(report, "ignored"));
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

AETHER_TEST(LocPo_RejectsEntriesMissingSourceOrTranslationFields) {
    StringTable table;
    table.Set("de", "quit", "Vorhanden");
    std::vector<std::string> errors;
    const std::string valid = "msgctxt \"play\"\nmsgid \"\"\nmsgstr \"Spielen\"\n\n";
    CHECK(!ImportPo(table, "de", valid + "msgctxt \"quit\"\nmsgstr \"Beenden\"", &errors));
    const auto has = [&](const char* key, const char* value) {
        const std::string* found = table.Find("de", key);
        return found != nullptr && *found == value;
    };
    CHECK(has("quit", "Vorhanden") && has("play", "Spielen"));
    CHECK(errors.size() == 1 && errors[0].find("line 5") == 0 && errors[0].find("msgid") != std::string::npos);

    for (const char* extra : {"", "\n\n", "\nmsgid \"Following\"\nmsgstr \"Neu\"\n"}) {
        errors.clear();
        CHECK(!ImportPo(table, "de", std::string("msgid \"Missing translation\"") + extra, &errors));
        CHECK(errors.size() == 1 && errors[0].find("line 1") == 0 && errors[0].find("msgstr") != std::string::npos);
        CHECK(table.Find("de", "Following") == nullptr);
    }
    errors.clear();
    CHECK(ImportPo(table, "de", "msgctxt \"quit\"\nmsgid \"Quit\"\nmsgstr \"\"\n", &errors));
    CHECK(errors.empty() && has("quit", "Vorhanden")); // empty but present
}

AETHER_TEST(LocPo_CommentsBelongToTheNextEntryWithoutBlankLines) {
    const char* po = R"PO(msgid ""
msgstr ""
"Language: fr\n"
msgctxt "approved"
msgid "Play"
msgstr "Jouer"
#, fuzzy
msgctxt "draft"
msgid "Quit"
msgstr "Brouillon"
msgctxt "before-old"
msgid "Continue"
msgstr ""
"Conti"
"nuer"
#~ msgctxt "removed"
#~ msgid "Removed"
#~ msgstr "Retire"
msgctxt "after-old"
msgid "Options"
msgstr "Options"
)PO";
    StringTable table;
    table.Set("fr", "draft", "Quitter");
    std::vector<std::string> errors;
    CHECK(PoLanguage(po) == "fr");
    CHECK(ImportPo(table, "fr", po, &errors));
    const auto has = [&](const char* key, const char* expected) {
        const std::string* text = table.Find("fr", key);
        return text != nullptr && *text == expected;
    };
    CHECK(has("approved", "Jouer") && has("draft", "Quitter"));
    CHECK(has("before-old", "Continuer") && has("after-old", "Options"));
    CHECK(table.Find("fr", "removed") == nullptr);
    CHECK(errors.size() == 1 && errors[0].find("'draft' is marked fuzzy") != std::string::npos);
}

AETHER_TEST(LocSync_UsesOnlyThePoHeaderToChooseTheTargetLanguage) {
    const stdfs::path root = Dir("po_language");
    SyncOptions options;
    options.content_dir = root / "Content";
    options.strings_file = options.content_dir / "Localization/strings.astrings";
    options.po_in = root / "incoming";
    Write(options.strings_file, "key,en\nsettings.language,Language\nmenu.play,Play\n");

    // There is no header here; this is a legitimate translated label.
    const std::string headerless = "msgctxt \"settings.language\"\nmsgid \"Language\"\n"
                                   "msgstr \"Language: English\"\n";
    CHECK(PoLanguage(headerless).empty());
    Write(options.po_in / "fr.PO", headerless);
    // Only the real header field counts, even with comments and wrapped quoted strings.
    const std::string catalog = "# Language: de\nmsgid \"\"\nmsgstr \"\"\n"
                                "\"X-Language: it\\n\"\n\"Language: pt\"\n\"_BR\\n\"\n\n"
                                "msgctxt \"menu.play\"\nmsgid \"Play\"\nmsgstr \"Jogar\"\n";
    CHECK(PoLanguage(catalog) == "pt-BR");
    Write(options.po_in / "reviewed.Po", catalog);
    Write(options.po_in / "template.POT", "not a returned catalog");

    const SyncReport report = SyncProject(options);
    CHECK(report.ok && report.po_imported == 2);
    StringTable table;
    CHECK(table.ImportCsv(Slurp(options.strings_file)));
    const auto* french = table.Find("fr", "settings.language");
    const auto* portuguese = table.Find("pt-BR", "menu.play");
    CHECK(french && *french == "Language: English");
    CHECK(portuguese && *portuguese == "Jogar");
    CHECK(table.Find("english", "settings.language") == nullptr && table.Find("de", "menu.play") == nullptr);
}

AETHER_TEST(LocSync_MissingPoSourceDoesNotWriteTheProjectTable) {
    const stdfs::path root = Dir("incomplete_po");
    SyncOptions options;
    options.content_dir = root / "Content";
    options.strings_file = options.content_dir / "Localization/strings.astrings";
    options.po_in = root / "incoming";
    options.po_out = root / "outgoing";
    const std::string before = "key,en,de\nplay,Play,Spielen\nquit,Quit,Schliessen\n";
    Write(options.strings_file, before);
    Write(options.po_in / "de.po",
          "msgctxt \"play\"\nmsgid \"Play\"\nmsgstr \"Starten\"\n\n"
          "msgctxt \"quit\"\nmsgstr \"Beenden\"\n");
    const SyncReport report = SyncProject(options);
    CHECK(!report.ok && report.po_imported == 0 && report.po_written == 0);
    CHECK(!report.messages.empty() && report.messages.back().find("msgid") != std::string::npos);
    CHECK(Slurp(options.strings_file) == before && !stdfs::exists(options.po_out));
}

AETHER_TEST(LocSync_InvalidContentRootDoesNotWriteOutputs) {
    const stdfs::path root = Dir("bad_content_root");
    SyncOptions options;
    options.content_dir = root / "missing" / "Content";
    options.strings_file = options.content_dir / "Localization/strings.astrings";
    options.languages = {"fr"};
    options.po_out = root / "outgoing";
    const SyncReport missing = SyncProject(options);
    CHECK(!missing.ok && !missing.messages.empty());
    CHECK(!stdfs::exists(root / "missing") && !stdfs::exists(options.po_out));

    options.content_dir = root / "not_a_folder";
    Write(options.content_dir, "plain text");
    options.strings_file = root / "outside.astrings";
    const SyncReport wrong_type = SyncProject(options);
    CHECK(!wrong_type.ok && !wrong_type.messages.empty());
    CHECK(!stdfs::exists(options.strings_file));

    // A real but empty Content directory is a valid project.
    options.content_dir = root / "empty";
    stdfs::create_directories(options.content_dir);
    const SyncReport empty = SyncProject(options);
    CHECK(empty.ok && empty.gathered.entries.empty() && stdfs::exists(options.strings_file));
}

#ifndef _WIN32
AETHER_TEST(LocSync_UnreadablePoDirectoryDoesNotWriteOutputs) {
    const stdfs::path root = Dir("unreadable_po_folder");
    SyncOptions options;
    options.content_dir = root / "Content";
    options.strings_file = options.content_dir / "Localization/strings.astrings";
    options.po_in = root / "incoming";
    options.po_out = root / "outgoing";
    const std::string before = "key,en,de\nplay,Play,Spielen\n";
    Write(options.strings_file, before);
    stdfs::create_directories(options.po_in);
    stdfs::permissions(options.po_in, stdfs::perms::owner_exec);
    CHECK(stdfs::is_directory(options.po_in)); // search/stat still works
    std::error_code listing_error;
    const stdfs::directory_iterator listing(options.po_in, listing_error);

    if (listing_error == std::errc::permission_denied) {
        const SyncReport report = SyncProject(options);
        CHECK(!report.ok && report.po_imported == 0 && report.po_written == 0);
        CHECK(!report.messages.empty() && report.messages.back().find(options.po_in.string()) != std::string::npos);
        CHECK(Slurp(options.strings_file) == before && !stdfs::exists(options.po_out));
    } // privileged runners can list regardless of the permission bits

    stdfs::permissions(options.po_in, stdfs::perms::owner_all);
    const SyncReport empty = SyncProject(options);
    CHECK(empty.ok && empty.po_imported == 0 && empty.po_written == 1);
    CHECK(stdfs::is_regular_file(options.po_out / "de.po"));
}

AETHER_TEST(LocSync_PoStatusErrorDoesNotWriteOutputs) {
    const stdfs::path root = Dir("po_status_error");
    SyncOptions options;
    options.content_dir = root / "Content";
    options.strings_file = options.content_dir / "Localization/strings.astrings";
    options.po_in = root / "incoming";
    options.po_out = root / "outgoing";
    const std::string before = "key,en,de\nplay,Play,Spielen\n";
    Write(options.strings_file, before);
    stdfs::create_directories(options.po_in);
    const auto loop = options.po_in / "loop.PO";
    stdfs::create_symlink(loop.filename(), loop);

    const SyncReport report = SyncProject(options);
    CHECK(!report.ok && report.po_imported == 0 && report.po_written == 0);
    CHECK(!report.messages.empty() && report.messages.back().find(loop.string()) != std::string::npos);
    CHECK(Slurp(options.strings_file) == before && !stdfs::exists(options.po_out));

    // Unrelated files don't participate in catalog status checks.
    stdfs::remove(loop);
    stdfs::create_symlink("loop.txt", options.po_in / "loop.txt");
    const SyncReport ignored = SyncProject(options);
    CHECK(ignored.ok && ignored.po_imported == 0 && ignored.po_written == 1);
}
#endif

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
    // A second run with nothing new must leave both finished files untouched.
    const auto po_path = root / "po/fr.po";
    const auto mark = stdfs::file_time_type::clock::now() - std::chrono::hours(48);
    stdfs::last_write_time(o.strings_file, mark);
    stdfs::last_write_time(po_path, mark);
    const auto csv_time = stdfs::last_write_time(o.strings_file);
    const auto po_time = stdfs::last_write_time(po_path);
    const std::string before = Slurp(o.strings_file), po_before = Slurp(root / "po/fr.po");
    r = SyncProject(o);
    CHECK(r.ok && r.merge.added == 0 && Slurp(o.strings_file) == before && Slurp(root / "po/fr.po") == po_before);
    CHECK(stdfs::last_write_time(o.strings_file) == csv_time);
    CHECK(stdfs::last_write_time(po_path) == po_time);
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
