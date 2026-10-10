#include "vfx_tools.h"

#include "project_host.h"

#include "aether/vfx/emitter.h"
#include "aether/vfx/simulation.h"

#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

namespace aether::mcp {

namespace {

using namespace detail;

constexpr const char* kExtension = ".avfx";

std::string ReadAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Every .avfx under Content/, as content-relative '/'-separated paths (hidden folders skipped).
std::vector<std::string> ListEffects(OpenProject& p) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(p.paths.content, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& path = it->path();
        if (it->is_directory(ec) && !path.filename().string().empty() && path.filename().string()[0] == '.') {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && path.extension() == kExtension) out.push_back(fs::relative(path, p.paths.content, ec).generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

Json DiagnosticsJson(const std::vector<vfx::EmitterDiagnostic>& diagnostics) {
    Json out = Json::array();
    for (const vfx::EmitterDiagnostic& d : diagnostics) out.push_back({{"code", d.code}, {"severity", d.error ? "error" : "warning"}, {"message", d.message}});
    return out;
}

bool HasError(const std::vector<vfx::EmitterDiagnostic>& diagnostics) {
    for (const vfx::EmitterDiagnostic& d : diagnostics) {
        if (d.error) return true;
    }
    return false;
}

// A system from `definition` (an object or its text) or `asset` (an .avfx path in the open project).
vfx::ParticleSystemAsset ReadSystem(AssetHost& host, const Json& args, std::string* from) {
    std::string text;
    if (args.contains("definition")) {
        const Json& d = args["definition"];
        if (d.is_object()) text = d.dump();
        else if (d.is_string()) text = d.get<std::string>();
        else throw ToolError("\"definition\" must be a JSON object or a string of JSON");
        if (from != nullptr) *from = "definition";
    } else if (args.contains("asset")) {
        OpenProject& p = host.Require();
        const std::string rel = CleanRelative(RequireString(args, "asset"));
        const fs::path file = p.paths.content / rel;
        std::error_code ec;
        if (rel.empty() || file.extension() != kExtension || !fs::is_regular_file(file, ec)) throw ToolError("No particle effect \"" + RequireString(args, "asset") + "\" (an .avfx under Content/)");
        text = ReadAll(file);
        if (from != nullptr) *from = rel;
    } else {
        throw ToolError("Give \"definition\" (the effect as JSON) or \"asset\" (an .avfx path in the open project)");
    }
    vfx::ParticleSystemAsset system;
    std::string error;
    if (!vfx::LoadParticleSystem(text, system, &error)) throw ToolError("Not a valid particle system: " + error);
    return system;
}

Json ToJson(const vfx::ParticleSystemAsset& system) { return Json::parse(vfx::SaveParticleSystem(system)); }

// An effect with every kind of thing in it a first effect needs: a burst and a stream, shape, velocity, size and colour,
// forces, size and colour over life, a sprite, and one parameter bound to the spawn rate.
vfx::ParticleSystemAsset SampleSystem() {
    using namespace vfx;
    ParticleSystemAsset system;
    system.name = "Sparks";
    Emitter e;
    e.settings.name = "Sparks";
    e.settings.max_particles = 500;
    e.settings.duration = 2.0f;
    e.settings.looping = true;
    SpawnRate rate;
    rate.rate = 60.0f;
    SpawnBurst burst;
    burst.count = FloatRange{20.0f, 20.0f};
    e.spawn = {rate, burst};
    InitLifetime life;
    life.seconds = FloatRange{0.6f, 1.2f};
    InitVelocity velocity;
    velocity.mode = VelocityMode::Cone;
    velocity.direction = Vec3(0.0f, 1.0f, 0.0f);
    velocity.cone_angle = 0.5f;
    velocity.speed = FloatRange{3.0f, 6.0f};
    InitSize size;
    size.size = FloatRange{0.05f, 0.12f};
    InitColor color;
    color.color = ColorGradient::Fade(LinearColor{1.0f, 0.55f, 0.1f, 1.0f}, LinearColor{1.0f, 0.9f, 0.4f, 1.0f});
    e.init = {life, velocity, size, color};
    Gravity gravity;
    Drag drag;
    drag.coefficient = 1.5f;
    SizeOverLife shrink;
    shrink.curve = FloatCurve::Line(1.0f, 0.0f);
    ColorOverLife fade;
    fade.gradient = ColorGradient::Fade(LinearColor{1, 1, 1, 1}, LinearColor{1, 1, 1, 0});
    e.update = {gravity, drag, shrink, fade};
    SpriteRenderer sprite;
    sprite.material = "Materials/spark";
    sprite.blend = BlendMode::Additive;
    sprite.sort = SortMode::None;
    e.render = {sprite};
    e.bindings.push_back({"Intensity", "spawn[0].rate"});
    system.emitters = {e};
    system.parameters.push_back({"Intensity", ParameterValue::Float(60.0f)});
    return system;
}

// A module's default JSON, by way of an emitter holding just that module.
template <typename Module>
Json ModuleDefaults(const std::string& name, const char* stage) {
    Module module;
    if (!vfx::MakeModule(name, module)) return nullptr;
    vfx::Emitter e;
    if constexpr (std::is_same_v<Module, vfx::SpawnModule>) e.spawn = {module};
    else if constexpr (std::is_same_v<Module, vfx::InitModule>) e.init = {module};
    else if constexpr (std::is_same_v<Module, vfx::UpdateModule>) e.update = {module};
    else e.render = {module};
    const Json j = vfx::EmitterToJson(e);
    if (j.contains(stage) && j[stage].is_array() && !j[stage].empty()) return j[stage][0];
    return nullptr;
}

Json ModuleCatalog() {
    Json out = Json::object();
    Json spawn = Json::object(), init = Json::object(), update = Json::object(), render = Json::object();
    for (const std::string& n : vfx::SpawnModuleNames()) spawn[n] = ModuleDefaults<vfx::SpawnModule>(n, "spawn");
    for (const std::string& n : vfx::InitModuleNames()) init[n] = ModuleDefaults<vfx::InitModule>(n, "init");
    for (const std::string& n : vfx::UpdateModuleNames()) update[n] = ModuleDefaults<vfx::UpdateModule>(n, "update");
    for (const std::string& n : vfx::RenderModuleNames()) render[n] = ModuleDefaults<vfx::RenderModule>(n, "render");
    out["spawn"] = spawn;
    out["init"] = init;
    out["update"] = update;
    out["render"] = render;
    return out;
}

Json Vec3Json(const Vec3& v) { return Json::array({v.x, v.y, v.z}); }

} // namespace

void RegisterVfxTools(McpServer& server, std::shared_ptr<AssetHost> host) {
    server.AddTool(
        {"vfx_schema",
         "The shape of a particle effect (.avfx): a sample effect with every kind of module, the notes, and the default fields of every "
         "module (spawn, init, update and render) so you can see what each takes. An emitter is a stack of modules run each frame: "
         "Spawn (how many are born), Initialize (each new particle's lifetime, place, velocity, size, colour), Update (forces, "
         "collision, size and colour over life), Render (sprites, meshes, ribbons, lights).",
         Schema(Json::object()), [](const Json&) -> Json {
             return {{"extension", kExtension},
                     {"sample", ToJson(SampleSystem())},
                     {"modules", ModuleCatalog()},
                     {"notes", {"An effect holds emitters that play together, and parameters (Float, Vector, Color) the game can set; a binding makes a "
                                "module field follow a parameter: {\"parameter\": \"Intensity\", \"field\": \"spawn[0].rate\"}.",
                                "Directions in Initialize modules are in the emitter's frame; Update module places are the emitter's too unless `world` is set.",
                                "settings.space: World (particles stay where born) | Local (they move with the emitter); duration is one loop in seconds.",
                                "Sprite blend: Alpha | Additive | Premultiplied | Opaque; sort: None | BackToFront | FrontToBack | OldestFirst | NewestFirst.",
                                "vfx_validate gives the FX001... diagnostics; vfx_simulate runs the effect headless and reports what it does.",
                                ".avfx is not an asset-database type: it has no GUID and is not cooked, and the player does not play ParticleSystem components."}}};
         }});

    server.AddTool(
        {"vfx_validate",
         "Check a particle effect with the VFX module's own diagnostics (FX001...: nothing spawns, bad lifetimes or ranges, sub-emitter loops, "
         "bindings to unknown parameters ...). Give the effect as `definition` or `asset` (an .avfx in the open project).",
         Schema({{"definition", {{"description", "The effect: a JSON object, or its text"}}}, {"asset", {{"type", "string"}, {"description", "An .avfx path under Content/"}}}}),
         [host](const Json& args) -> Json {
             std::string from;
             const vfx::ParticleSystemAsset system = ReadSystem(*host, args, &from);
             const std::vector<vfx::EmitterDiagnostic> diagnostics = vfx::ValidateParticleSystem(system);
             return {{"ok", !HasError(diagnostics)}, {"asset", from}, {"emitters", system.emitters.size()}, {"diagnostics", DiagnosticsJson(diagnostics)}};
         }});

    server.AddTool(
        {"vfx_put",
         "Create or replace a particle effect file under the open project's Content/ (the .avfx extension is added). It must parse and have no "
         "error diagnostics; warnings are returned. Saved normalized.",
         Schema({{"path", {{"type", "string"}, {"description", "Relative to Content/, e.g. VFX/Sparks"}}},
                 {"definition", {{"description", "The effect: a JSON object, or its text"}}},
                 {"overwrite", {{"type", "boolean"}}}},
                {"path", "definition"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             std::string rel = CleanRelative(RequireString(args, "path"));
             if (rel.empty()) throw ToolError("\"path\" must name a file under Content/");
             if (fs::path(rel).extension() != kExtension) rel += kExtension;
             const vfx::ParticleSystemAsset system = ReadSystem(*host, {{"definition", args["definition"]}}, nullptr);
             const std::vector<vfx::EmitterDiagnostic> diagnostics = vfx::ValidateParticleSystem(system);
             if (HasError(diagnostics)) throw ToolError("Invalid effect: " + DiagnosticsJson(diagnostics).dump());
             const fs::path file = p.paths.content / rel;
             std::error_code ec;
             if (fs::exists(file, ec) && !OptBool(args, "overwrite", false)) throw ToolError(rel + " already exists (pass overwrite)");
             fs::create_directories(file.parent_path(), ec);
             {
                 std::ofstream out(file, std::ios::binary | std::ios::trunc);
                 const std::string text = vfx::SaveParticleSystem(system) + "\n";
                 out.write(text.data(), static_cast<std::streamsize>(text.size()));
                 if (!out) throw ToolError("Could not write " + rel);
             }
             return {{"path", rel}, {"name", system.name}, {"emitters", system.emitters.size()}, {"warnings", DiagnosticsJson(diagnostics)}};
         }});

    server.AddTool({"vfx_get", "Read a particle effect from the open project, as JSON.",
                    Schema({{"asset", {{"type", "string"}, {"description", "An .avfx path under Content/"}}}}, {"asset"}), [host](const Json& args) -> Json {
                        std::string from;
                        const vfx::ParticleSystemAsset system = ReadSystem(*host, args, &from);
                        return {{"path", from}, {"definition", ToJson(system)}};
                    }});

    server.AddTool({"vfx_list", "The particle effects (.avfx) in the open project: path, name, emitter count and whether each parses and validates.",
                    Schema(Json::object()), [host](const Json&) -> Json {
                        OpenProject& p = host->Require();
                        Json out = Json::array();
                        for (const std::string& rel : ListEffects(p)) {
                            vfx::ParticleSystemAsset system;
                            std::string error;
                            Json row = {{"path", rel}};
                            if (!vfx::LoadParticleSystem(ReadAll(p.paths.content / rel), system, &error)) {
                                row["valid"] = false;
                                row["error"] = error;
                            } else {
                                const std::vector<vfx::EmitterDiagnostic> d = vfx::ValidateParticleSystem(system);
                                row["name"] = system.name;
                                row["emitters"] = system.emitters.size();
                                row["valid"] = !HasError(d);
                                if (!d.empty()) row["diagnostics"] = DiagnosticsJson(d);
                            }
                            out.push_back(row);
                        }
                        return out;
                    }});

    server.AddTool(
        {"vfx_check",
         "Check every particle effect in the open project (it must parse and have no error diagnostics) and that every ParticleSystem component in "
         "its scenes and prefabs names an .avfx that exists.",
         Schema(Json::object()), [host](const Json&) -> Json {
             OpenProject& p = host->Require();
             Json problems = Json::array(), warnings = Json::array();
             const std::vector<std::string> effects = ListEffects(p);
             const std::set<std::string> existing(effects.begin(), effects.end());
             for (const std::string& rel : effects) {
                 vfx::ParticleSystemAsset system;
                 std::string error;
                 if (!vfx::LoadParticleSystem(ReadAll(p.paths.content / rel), system, &error)) {
                     problems.push_back({{"path", rel}, {"problem", error}});
                     continue;
                 }
                 for (const vfx::EmitterDiagnostic& d : vfx::ValidateParticleSystem(system)) {
                     (d.error ? problems : warnings).push_back({{"path", rel}, {d.error ? "problem" : "warning", d.code + ": " + d.message}});
                 }
             }
             unsigned scene_uses = 0;
             for (const AssetRecord* r : p.database->All()) {
                 if (r->IsSubAsset() || (r->importer != "Scene" && r->importer != "Prefab")) continue;
                 const fs::path file = p.database->SourcePath(r->guid);
                 if (file.extension() == ".aesc") continue;
                 const Json doc = Json::parse(ReadAll(file), nullptr, false);
                 if (!doc.is_object() || !doc.contains("entities") || !doc["entities"].is_array()) continue;
                 for (const Json& entity : doc["entities"]) {
                     if (!entity.is_object() || !entity.contains("components") || !entity["components"].is_object()) continue;
                     const Json& comps = entity["components"];
                     if (!comps.contains("ParticleSystem") || !comps["ParticleSystem"].is_object()) continue;
                     ++scene_uses;
                     const std::string asset = comps["ParticleSystem"].value("asset", std::string());
                     if (!asset.empty() && existing.count(asset) == 0) problems.push_back({{"path", r->path}, {"problem", "a ParticleSystem's asset \"" + asset + "\" is not an .avfx in the project"}});
                 }
             }
             return {{"ok", problems.empty()}, {"effects", effects.size()}, {"scene_uses", scene_uses}, {"problems", problems}, {"warnings", warnings}};
         }});

    server.AddTool(
        {"vfx_simulate",
         "Run a particle effect headless and report what it does, with no GPU: live particle counts over time per emitter, the peak, how many were "
         "born in all, when it finished (a one-shot) or whether it loops on, the bounds of what is alive at the end, and a sample of live "
         "particles (position, size, colour, age). The same effect, seed and steps always give the same result. `parameters` sets the effect's "
         "declared parameters first, e.g. {\"Intensity\": 120}.",
         Schema({{"definition", {{"description", "The effect: a JSON object, or its text"}}},
                 {"asset", {{"type", "string"}, {"description", "An .avfx path under Content/"}}},
                 {"seconds", {{"type", "number"}, {"description", "How long to run, up to 30 (default 3)"}}},
                 {"dt", {{"type", "number"}, {"description", "Step in seconds, 1/240 to 1/10 (default 1/60)"}}},
                 {"seed", {{"type", "integer"}}},
                 {"samples", {{"type", "integer"}, {"description", "Time samples to report, 1-60 (default 10)"}}},
                 {"parameters", {{"type", "object"}, {"description", "Declared parameter -> number, [x,y,z] or [r,g,b,a]"}}}}),
         [host](const Json& args) -> Json {
             std::string from;
             const vfx::ParticleSystemAsset system = ReadSystem(*host, args, &from);
             const std::vector<vfx::EmitterDiagnostic> diagnostics = vfx::ValidateParticleSystem(system);
             if (HasError(diagnostics)) return {{"ok", false}, {"asset", from}, {"diagnostics", DiagnosticsJson(diagnostics)}};

             auto number = [&](const char* key, double fallback, double lo, double hi) {
                 if (!args.contains(key)) return fallback;
                 if (!args[key].is_number() || !(args[key].get<double>() >= lo) || !(args[key].get<double>() <= hi)) {
                     throw ToolError(std::string("\"") + key + "\" must be a number from " + std::to_string(lo) + " to " + std::to_string(hi));
                 }
                 return args[key].get<double>();
             };
             const double seconds = number("seconds", 3.0, 0.01, 30.0);
             const double dt = number("dt", 1.0 / 60.0, 1.0 / 240.0, 0.1);
             const unsigned samples = static_cast<unsigned>(number("samples", 10, 1, 60));
             u64 seed = 1;
             if (args.contains("seed")) {
                 if (!args["seed"].is_number_integer()) throw ToolError("\"seed\" must be an integer");
                 seed = static_cast<u64>(args["seed"].get<long long>());
             }

             vfx::ParticleSystemInstance instance(system, seed);
             if (args.contains("parameters")) {
                 if (!args["parameters"].is_object()) throw ToolError("\"parameters\" must be an object");
                 for (auto& [name, value] : args["parameters"].items()) {
                     const vfx::ParticleParameter* declared = system.FindParameter(name);
                     if (declared == nullptr) throw ToolError("The effect declares no parameter \"" + name + "\"");
                     vfx::ParameterValue v;
                     if (declared->value.type == vfx::ParameterType::Float) {
                         if (!value.is_number()) throw ToolError("Parameter \"" + name + "\" is a number");
                         v = vfx::ParameterValue::Float(value.get<f32>());
                     } else if (declared->value.type == vfx::ParameterType::Vector) {
                         if (!value.is_array() || value.size() != 3) throw ToolError("Parameter \"" + name + "\" is [x, y, z]");
                         v = vfx::ParameterValue::Vector(Vec3(value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>()));
                     } else {
                         if (!value.is_array() || value.size() != 4) throw ToolError("Parameter \"" + name + "\" is [r, g, b, a]");
                         v = vfx::ParameterValue::Color(vfx::LinearColor{value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>(), value[3].get<f32>()});
                     }
                     std::string error;
                     if (!instance.SetParameter(name, v, &error)) throw ToolError("Could not set \"" + name + "\": " + error);
                 }
             }
             vfx::EmitterPose pose;
             pose.rotation = Quaternion::Identity();
             instance.Teleport(pose);

             const usize emitters = instance.EmitterCount();
             const unsigned steps = static_cast<unsigned>(std::ceil(seconds / dt));
             const unsigned every = std::max(1u, steps / samples);
             std::vector<usize> peak(emitters, 0);
             usize peak_total = 0;
             double finished_at = -1.0;
             Json series = Json::array();
             double time = 0.0;
             for (unsigned i = 1; i <= steps; ++i) {
                 instance.Update(static_cast<f32>(dt));
                 time += dt;
                 for (usize e = 0; e < emitters; ++e) peak[e] = std::max(peak[e], instance.Emitter(e).Count());
                 peak_total = std::max(peak_total, instance.Count());
                 if (finished_at < 0.0 && instance.Finished()) finished_at = time;
                 if (i % every == 0 || i == steps) {
                     Json counts = Json::array();
                     for (usize e = 0; e < emitters; ++e) counts.push_back(instance.Emitter(e).Count());
                     series.push_back({{"time", time}, {"alive", instance.Count()}, {"per_emitter", counts}});
                 }
             }

             Json per_emitter = Json::array();
             for (usize e = 0; e < emitters; ++e) {
                 const vfx::EmitterInstance& em = instance.Emitter(e);
                 Json row = {{"name", em.Asset().settings.name}, {"alive", em.Count()}, {"peak", peak[e]}, {"born", em.TotalSpawned()}, {"loops", em.Loop()}};
                 const vfx::Bounds b = em.ComputeBounds();
                 if (b.valid) row["bounds"] = {{"min", Vec3Json(b.min)}, {"max", Vec3Json(b.max)}};
                 Json sample = Json::array();
                 const vfx::ParticleBuffer& buf = em.Particles();
                 const usize stride = std::max<usize>(1, buf.count / 6);
                 for (usize k = 0; k < buf.count && sample.size() < 6; k += stride) {
                     sample.push_back({{"position", Vec3Json(buf.position[k])}, {"size", buf.size[k]},
                                       {"color", Json::array({buf.color[k].r, buf.color[k].g, buf.color[k].b, buf.color[k].a})},
                                       {"age", buf.age[k]}, {"lifetime", buf.lifetime[k]}});
                 }
                 if (!sample.empty()) row["sample_particles"] = sample;
                 per_emitter.push_back(row);
             }
             Json out = {{"ok", true}, {"asset", from}, {"seconds", seconds}, {"dt", dt}, {"seed", seed},
                         {"peak_alive", peak_total}, {"alive_at_end", instance.Count()}, {"sub_emitter_spawns", instance.SubEmitterSpawns()},
                         {"emitters", per_emitter}, {"timeline", series}, {"warnings", DiagnosticsJson(diagnostics)}};
             out["finished_at"] = finished_at >= 0.0 ? Json(finished_at) : Json(nullptr);
             out["loops_on"] = finished_at < 0.0;
             return out;
         }});
}

} // namespace aether::mcp
