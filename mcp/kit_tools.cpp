#include "kit_tools.h"

#include "project_host.h"

#include "aether/gameplay/gameplay_ability.h"
#include "aether/gameplay/gameplay_effect.h"
#include "aether/input/actions.h"
#include "aether/input/bindings.h"
#include "aether/reflection/serialize.h"

#if AETHER_MCP_INVENTORY
#include "aether/inventory/item_def.h"
#endif
#if AETHER_MCP_QUESTS
#include "aether/quests/quest_def.h"
#endif

#include <atomic>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace aether::mcp {

namespace {

using namespace detail;

// One kind of kit definition: its file type, a blank one to learn the shape from, and the kit's own
// parser and validator (the ones the game loads it with).
struct KitType {
    std::string name;        // the tool-facing name: "effect"
    std::string extension;   // ".aeffect"
    std::string importer;    // the asset database's name for it
    std::string description;
    std::function<Json()> blank;
    // Empty string if `text` parses and validates; else the kit's error. `canonical` gets the normalized JSON.
    std::function<std::string(const std::string& text, Json& canonical)> check;
};

Json Wrapped(const char* type, Json data) { return {{"$type", type}, {"data", std::move(data)}}; }

// The input loaders read files: check text by way of a temporary one.
template <typename T, typename Loader>
std::string CheckViaFile(const std::string& text, const char* extension, Loader load, Json& canonical) {
    static std::atomic<unsigned> counter{0};
    const fs::path file = fs::temp_directory_path() / ("aether_mcp_kit_" + std::to_string(++counter) + extension);
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    }
    T value;
    std::string error;
    const bool ok = load(file, value, &error);
    std::error_code ec;
    fs::remove(file, ec);
    if (!ok) return error.empty() ? "Not a valid definition" : error;
    canonical = Json::parse(text, nullptr, false);
    return {};
}

const std::vector<KitType>& Types() {
    static const std::vector<KitType> types = [] {
        std::vector<KitType> t;
        t.push_back({"effect", ".aeffect", "GameplayEffect",
                     "A change to an entity's attributes, instant or lasting, with the tags it needs and grants.",
                     [] { return gas::EffectToJson(gas::GameplayEffect{}); },
                     [](const std::string& text, Json& canonical) -> std::string {
                         gas::GameplayEffect e;
                         std::string error;
                         if (!gas::EffectFromJson(text, e, &error)) return error;
                         canonical = gas::EffectToJson(e);
                         return {};
                     }});
        t.push_back({"ability", ".aability", "GameplayAbility",
                     "Something an entity can do (a jump, a fireball), gated by tags, paid for with an effect, put on cooldown by another.",
                     [] { return gas::AbilityToJson(gas::GameplayAbility{}); },
                     [](const std::string& text, Json& canonical) -> std::string {
                         gas::GameplayAbility a;
                         std::string error;
                         if (!gas::AbilityFromJson(text, a, &error)) return error;
                         canonical = gas::AbilityToJson(a);
                         return {};
                     }});
#if AETHER_MCP_INVENTORY
        t.push_back({"item", ".aitem", "ItemDefinition", "What a kind of item is: stacking, weight, tags, equip slot, what it does when used.",
                     [] { return inv::ItemToJson(inv::ItemDef{}); },
                     [](const std::string& text, Json& canonical) -> std::string {
                         inv::ItemDef i;
                         std::string error;
                         if (!inv::ItemFromJson(text, i, &error)) return error;
                         canonical = inv::ItemToJson(i);
                         return {};
                     }});
#endif
#if AETHER_MCP_QUESTS
        t.push_back({"quest", ".aquest", "QuestDefinition", "A quest: objectives, prerequisites and rewards.",
                     [] { return quest::QuestToJson(quest::QuestDef{}); },
                     [](const std::string& text, Json& canonical) -> std::string {
                         quest::QuestDef q;
                         std::string error;
                         if (!quest::QuestFromJson(text, q, &error)) return error;
                         canonical = quest::QuestToJson(q);
                         return {};
                     }});
#endif
        t.push_back({"input_action", ".aaction", "InputAction", "A named input action (Jump, Move): its value type.",
                     [] { return Wrapped("InputAction", reflect::ToJson(input::InputAction{})); },
                     [](const std::string& text, Json& canonical) {
                         return CheckViaFile<input::InputAction>(text, ".aaction", input::LoadInputAction, canonical);
                     }});
        t.push_back({"input_mapping", ".amapping", "InputMapping", "A mapping context: which keys and buttons drive which actions, with modifiers and triggers.",
                     [] { return Wrapped("InputMappingContext", reflect::ToJson(input::InputMappingContext{})); },
                     [](const std::string& text, Json& canonical) {
                         return CheckViaFile<input::InputMappingContext>(text, ".amapping", input::LoadMappingContext, canonical);
                     }});
        return t;
    }();
    return types;
}

const KitType& RequireType(const std::string& name) {
    for (const KitType& t : Types()) {
        if (t.name == name) return t;
    }
    std::string known;
    for (const KitType& t : Types()) known += (known.empty() ? "" : ", ") + t.name;
    throw ToolError("Unknown kit type \"" + name + "\". Known: " + known);
}

const KitType* TypeOfImporter(const std::string& importer) {
    for (const KitType& t : Types()) {
        if (t.importer == importer) return &t;
    }
    return nullptr;
}

// A definition with every field set, so the shape is visible (the "blank" one shows only what is not
// default). Built from the kit's own structs, so it is always something the kit accepts.
Json SampleOf(const std::string& type) {
    using gas::GameplayTag;
    using gas::TagQuery;
    if (type == "effect") {
        gas::GameplayEffect e;
        e.name = "Regeneration";
        e.duration_policy = gas::GameplayEffect::Duration::Timed;
        e.duration = 10.0f;
        e.modifiers = {{"Health", gas::GameplayEffect::Op::Add, 5.0f}};
        e.stacking = gas::GameplayEffect::Stacking::StackCount;
        e.max_stacks = 3;
        e.per_source = true;
        e.period = 1.0f;
        e.execute_on_apply = true;
        e.require = TagQuery::All({GameplayTag::Make("State.Alive")});
        e.blocked = TagQuery::Any({GameplayTag::Make("State.Poisoned")});
        e.granted_tags = {GameplayTag::Make("State.Regenerating")};
        e.remove_on_tags = {GameplayTag::Make("State.Dead")};
        return gas::EffectToJson(e);
    }
    if (type == "ability") {
        gas::GameplayAbility a;
        a.name = "Fireball";
        a.tags = {GameplayTag::Make("Ability.Spell")};
        a.activation_required = TagQuery::All({GameplayTag::Make("State.Alive")});
        a.activation_blocked = TagQuery::Any({GameplayTag::Make("State.Stunned")});
        a.cancel_abilities_with_tags = TagQuery::Any({GameplayTag::Make("Ability.Channel")});
        a.block_abilities_with_tags = TagQuery::Any({GameplayTag::Make("Ability.Spell")});
        a.activation_owned_tags = {GameplayTag::Make("State.Casting")};
        a.cost = "FireballCost";
        a.cooldown = "FireballCooldown";
        a.max_duration = 2.0f;
        a.commit_on_activate = false;
        return gas::AbilityToJson(a);
    }
#if AETHER_MCP_INVENTORY
    if (type == "item") {
        inv::ItemDef i;
        i.name = "HealthPotion";
        i.display_key = "item.health_potion";
        i.icon = "Textures/potion.png";
        i.max_stack = 10;
        i.weight = 0.5f;
        i.tags = {GameplayTag::Make("Item.Consumable")};
        i.equip_slot = "Belt";
        i.equip_effects = {"BeltBuff"};
        i.use_effect = "Heal";
        i.consume_on_use = true;
        return inv::ItemToJson(i);
    }
#endif
#if AETHER_MCP_QUESTS
    if (type == "quest") {
        quest::QuestDef q;
        q.name = "ClearTheCellar";
        q.title = "Clear the cellar";
        q.title_key = "quest.cellar.title";
        q.description = "Something is down there.";
        q.description_key = "quest.cellar.description";
        q.objectives = {{"rats", "Kill the rats", "quest.cellar.rats", quest::Objective::Kind::Count, "Rat", 5, false},
                        {"key", "Find the key", "", quest::Objective::Kind::Flag, "CellarKey", 1, true}};
        q.prerequisites = {"MeetTheInnkeeper"};
        q.reward_effects = {"Heal"};
        q.reward_items = {{"HealthPotion", 2}};
        return quest::QuestToJson(q);
    }
#endif
    return Json();
}

// What the JSON does not say: the allowed words, and how definitions refer to each other.
std::vector<std::string> NotesOf(const std::string& type) {
    if (type == "effect") {
        return {"duration_policy: instant | timed | infinite (timed needs duration > 0)",
                "modifiers[].op: add | multiply | override",
                "stacking: none | refresh | stack (stack uses max_stacks)",
                "period > 0 applies the modifiers every period seconds (not on instant effects)",
                "require / blocked are tag queries: {\"op\":\"all|any|none\",\"tags\":[...]} or {\"op\":\"and|or\",\"children\":[...]}",
                "Tag names are dotted: State.Stunned. Tags match hierarchically."};
    }
    if (type == "ability") {
        return {"cost names an instant effect, cooldown a timed effect: both by effect name (kit_check verifies they exist)",
                "the *_tags queries use the same format as an effect's require / blocked"};
    }
    if (type == "item") return {"use_effect and equip_effects name effects (kit_check verifies them)", "max_stack >= 1"};
    if (type == "quest") {
        return {"objectives[].kind: count | tag | flag; ids unique; required >= 1",
                "prerequisites name quests; reward_effects name effects; reward_items[].item names an item (kit_check verifies them)"};
    }
    if (type == "input_action") return {"value_type: Bool | Axis1D | Axis2D | Axis3D"};
    if (type == "input_mapping") return {"bindings[] map a key to an action with optional modifiers (DeadZone, Negate, Swizzle, Scale) and triggers; see the example"};
    return {};
}

// The definition argument: a JSON object, or the file's text.
std::string DefinitionText(const Json& args) {
    if (!args.contains("definition")) throw ToolError("Missing argument \"definition\" (the definition as a JSON object)");
    const Json& d = args["definition"];
    if (d.is_object()) return d.dump(2);
    if (d.is_string()) return d.get<std::string>();
    throw ToolError("\"definition\" must be a JSON object or a string of JSON");
}

std::string ReadText(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// "name" of a definition, for listings and cross-references.
std::string NameOf(const Json& canonical) {
    const Json& body = canonical.is_object() && canonical.contains("data") && canonical["data"].is_object() ? canonical["data"] : canonical;
    return body.is_object() && body.contains("name") && body["name"].is_string() ? body["name"].get<std::string>() : std::string();
}

} // namespace

void RegisterKitTools(McpServer& server, std::shared_ptr<detail::AssetHost> host) {
    server.AddTool({"kit_types",
                    "The gameplay-kit definition types: file extension, what each is, and how many the open project has. Use kit_schema for the "
                    "shape of one.",
                    Schema(Json::object()), [host](const Json&) -> Json {
                        Json out = Json::array();
                        for (const KitType& t : Types()) {
                            Json row = {{"type", t.name}, {"extension", t.extension}, {"importer", t.importer}, {"description", t.description}};
                            if (host->project) {
                                usize n = 0;
                                for (const AssetRecord* r : host->project->database->All()) {
                                    if (!r->IsSubAsset() && r->importer == t.importer) ++n;
                                }
                                row["in_project"] = n;
                            }
                            out.push_back(row);
                        }
                        return out;
                    }});

    server.AddTool({"kit_schema",
                    "The shape of one definition type: a sample with every field set, the allowed words and cross-reference rules (notes), "
                    "the blank definition, and (if a project is open) an existing example from it.",
                    Schema({{"type", {{"type", "string"}, {"description", "effect, ability, item, quest, input_action or input_mapping"}}}}, {"type"}),
                    [host](const Json& args) -> Json {
                        const KitType& t = RequireType(RequireString(args, "type"));
                        Json out = {{"type", t.name}, {"extension", t.extension}, {"description", t.description}, {"blank", t.blank()}, {"notes", NotesOf(t.name)}};
                        Json sample = SampleOf(t.name);
                        if (!sample.is_null()) out["sample"] = sample;
                        if (host->project) {
                            for (const AssetRecord* r : host->project->database->All()) {
                                if (r->IsSubAsset() || r->importer != t.importer) continue;
                                const fs::path file = host->project->database->SourcePath(r->guid);
                                Json parsed = Json::parse(ReadText(file), nullptr, false);
                                if (!parsed.is_discarded()) {
                                    out["example"] = {{"path", r->path}, {"definition", parsed}};
                                    break;
                                }
                            }
                        }
                        return out;
                    }});

    server.AddTool({"kit_validate",
                    "Check a definition with the kit's own parser and validator without writing anything. Returns ok, or the kit's named error "
                    "(e.g. \"effect.name_empty: ...\").",
                    Schema({{"type", {{"type", "string"}}}, {"definition", {{"description", "The definition: a JSON object, or its text"}}}}, {"type", "definition"}),
                    [](const Json& args) -> Json {
                        const KitType& t = RequireType(RequireString(args, "type"));
                        Json canonical;
                        const std::string error = t.check(DefinitionText(args), canonical);
                        if (!error.empty()) return {{"ok", false}, {"error", error}};
                        return {{"ok", true}, {"normalized", canonical}};
                    }});

    server.AddTool({"kit_list",
                    "The kit definitions in the open project (all types, or one): path, definition name and whether it parses and validates.",
                    Schema({{"type", {{"type", "string"}, {"description", "Only this type"}}}}), [host](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        const KitType* only = args.contains("type") ? &RequireType(RequireString(args, "type")) : nullptr;
                        Json out = Json::array();
                        for (const AssetRecord* r : p.database->All()) {
                            if (r->IsSubAsset()) continue;
                            const KitType* t = TypeOfImporter(r->importer);
                            if (t == nullptr || (only != nullptr && t != only)) continue;
                            Json canonical;
                            const std::string text = ReadText(p.database->SourcePath(r->guid));
                            const std::string error = t->check(text, canonical);
                            Json row = {{"type", t->name}, {"path", r->path}, {"name", NameOf(canonical)}, {"valid", error.empty()}};
                            if (!error.empty()) row["error"] = error;
                            out.push_back(row);
                        }
                        return out;
                    }});

    server.AddTool({"kit_get", "Read one definition from the open project, as JSON.",
                    Schema({{"asset", {{"type", "string"}, {"description", "Content path or GUID"}}}}, {"asset"}), [host](const Json& args) -> Json {
                        OpenProject& p = host->Require();
                        const AssetRecord& r = FindAsset(p, RequireString(args, "asset"));
                        const KitType* t = TypeOfImporter(r.importer);
                        if (t == nullptr) throw ToolError(r.path + " is not a kit definition (it is a " + r.importer + ")");
                        Json parsed = Json::parse(ReadText(p.database->SourcePath(r.guid)), nullptr, false);
                        if (parsed.is_discarded()) throw ToolError(r.path + " is not valid JSON");
                        return {{"type", t->name}, {"path", r.path}, {"definition", parsed}};
                    }});

    server.AddTool(
        {"kit_put",
         "Create or replace a definition in the open project: validated with the kit's parser first (nothing is written if it is invalid), "
         "then saved as normalized JSON with the right extension and scanned so it has a GUID.",
         Schema({{"type", {{"type", "string"}}},
                 {"path", {{"type", "string"}, {"description", "Under Content/, e.g. Gameplay/Effects/Heal (the extension is added)"}}},
                 {"definition", {{"description", "The definition: a JSON object, or its text"}}},
                 {"overwrite", {{"type", "boolean"}, {"description", "Replace an existing file (default false)"}}}},
                {"type", "path", "definition"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const KitType& t = RequireType(RequireString(args, "type"));
             std::string rel = CleanRelative(RequireString(args, "path"));
             if (rel.empty()) throw ToolError("\"path\" must name a file under Content/");
             if (fs::path(rel).extension() != t.extension) rel += t.extension;
             Json canonical;
             const std::string error = t.check(DefinitionText(args), canonical);
             if (!error.empty()) throw ToolError("Invalid " + t.name + ": " + error);

             const fs::path file = p.paths.content / rel;
             std::error_code ec;
             if (fs::exists(file, ec) && !OptBool(args, "overwrite", false)) throw ToolError(rel + " already exists (pass overwrite)");
             fs::create_directories(file.parent_path(), ec);
             {
                 std::ofstream out(file, std::ios::binary | std::ios::trunc);
                 const std::string text = canonical.dump(2) + "\n";
                 out.write(text.data(), static_cast<std::streamsize>(text.size()));
                 if (!out) throw ToolError("Could not write " + rel);
             }
             p.database->Scan();
             Json out = {{"type", t.name}, {"path", rel}, {"name", NameOf(canonical)}};
             if (const AssetRecord* r = p.database->FindByPath(rel)) out["guid"] = assets::ToString(r->guid);
             return out;
         }});

    server.AddTool(
        {"kit_check",
         "Check every kit definition in the open project: each must parse and validate, and cross-references must resolve (an ability's cost "
         "and cooldown effects, an item's use and equip effects, a quest's reward effects, reward items and prerequisites). Also "
         "reports duplicate names.",
         Schema(Json::object()), [host](const Json&) -> Json {
             OpenProject& p = host->Require();
             struct Def {
                 const KitType* type;
                 std::string path;
                 Json canonical;
             };
             std::vector<Def> defs;
             Json problems = Json::array();
             auto problem = [&](const std::string& path, const std::string& message) { problems.push_back({{"path", path}, {"problem", message}}); };

             for (const AssetRecord* r : p.database->All()) {
                 if (r->IsSubAsset()) continue;
                 const KitType* t = TypeOfImporter(r->importer);
                 if (t == nullptr) continue;
                 Json canonical;
                 const std::string error = t->check(ReadText(p.database->SourcePath(r->guid)), canonical);
                 if (!error.empty()) {
                     problem(r->path, error);
                     continue;
                 }
                 defs.push_back({t, r->path, canonical});
             }

             std::map<std::string, std::set<std::string>> names; // type -> names
             std::map<std::pair<std::string, std::string>, std::string> first_seen;
             for (const Def& d : defs) {
                 const std::string name = NameOf(d.canonical);
                 if (name.empty()) continue;
                 if (!names[d.type->name].insert(name).second) {
                     problem(d.path, "duplicate " + d.type->name + " name \"" + name + "\" (also in " + first_seen[{d.type->name, name}] + ")");
                 } else {
                     first_seen[{d.type->name, name}] = d.path;
                 }
             }

             auto need = [&](const Def& d, const char* kind, const std::string& what, const std::string& reference) {
                 if (reference.empty()) return;
                 if (names[kind].count(reference) == 0) problem(d.path, what + " \"" + reference + "\" is not a " + kind + " in the project");
             };
             auto strings = [](const Json& j, const char* key) {
                 std::vector<std::string> out;
                 if (j.is_object() && j.contains(key) && j[key].is_array()) {
                     for (const Json& v : j[key]) {
                         if (v.is_string()) out.push_back(v.get<std::string>());
                     }
                 }
                 return out;
             };
             for (const Def& d : defs) {
                 const Json& j = d.canonical;
                 const std::string& kind = d.type->name;
                 if (kind == "ability") {
                     need(d, "effect", "cost", j.value("cost", std::string()));
                     need(d, "effect", "cooldown", j.value("cooldown", std::string()));
                 } else if (kind == "item") {
                     need(d, "effect", "use_effect", j.value("use_effect", std::string()));
                     for (const std::string& e : strings(j, "equip_effects")) need(d, "effect", "equip effect", e);
                 } else if (kind == "quest") {
                     for (const std::string& e : strings(j, "reward_effects")) need(d, "effect", "reward effect", e);
                     for (const std::string& q : strings(j, "prerequisites")) need(d, "quest", "prerequisite", q);
                     if (j.contains("reward_items") && j["reward_items"].is_array()) {
                         for (const Json& r : j["reward_items"]) need(d, "item", "reward item", r.value("item", std::string()));
                     }
                 }
             }

             Json counts = Json::object();
             for (const Def& d : defs) counts[d.type->name] = counts.value(d.type->name, 0) + 1;
             return {{"ok", problems.empty()}, {"definitions", defs.size()}, {"by_type", counts}, {"problems", problems}};
         }});
}

} // namespace aether::mcp
