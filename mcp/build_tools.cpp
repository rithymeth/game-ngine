#include "build_tools.h"

#include "process.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>

namespace aether::mcp {

namespace fs = std::filesystem;

namespace {

constexpr unsigned kDefaultBuildSeconds = 3600;
constexpr unsigned kDefaultTestSeconds = 1200;
constexpr unsigned kDefaultRunSeconds = 120;
constexpr unsigned kMaxSeconds = 4 * 3600;
constexpr std::size_t kTailChars = 6000;

std::string CacheValue(const fs::path& cache, const std::string& key) {
    std::ifstream in(cache);
    std::string line;
    const std::string prefix = key + ":";
    while (std::getline(in, line)) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            const std::size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string value = line.substr(eq + 1);
                while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
                return value;
            }
        }
    }
    return {};
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string Tail(const std::string& text, std::size_t chars = kTailChars) {
    if (text.size() <= chars) return text;
    std::string t = text.substr(text.size() - chars);
    const std::size_t nl = t.find('\n');
    if (nl != std::string::npos) t.erase(0, nl + 1); // start on a whole line
    return "...\n" + t;
}

unsigned Seconds(const Json& args, unsigned fallback) {
    if (!args.contains("timeout_seconds")) return fallback;
    if (!args["timeout_seconds"].is_number_integer()) throw ToolError("\"timeout_seconds\" must be an integer");
    const long long s = args["timeout_seconds"].get<long long>();
    if (s < 1 || s > kMaxSeconds) throw ToolError("\"timeout_seconds\" must be between 1 and 14400");
    return static_cast<unsigned>(s);
}

Json Schema(Json properties, std::vector<std::string> required = {}) {
    Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) schema["required"] = required;
    return schema;
}

std::vector<std::string> StringArray(const Json& args, const char* key) {
    std::vector<std::string> out;
    if (!args.contains(key)) return out;
    if (!args[key].is_array()) throw ToolError(std::string("\"") + key + "\" must be an array of strings");
    for (const Json& v : args[key]) {
        if (!v.is_string()) throw ToolError(std::string("\"") + key + "\" must be an array of strings");
        out.push_back(v.get<std::string>());
    }
    return out;
}

// The state tools share: where the build is and the compiler environment to run it in.
class BuildHost {
public:
    explicit BuildHost(BuildContext context) : context_(std::move(context)) {}

    const BuildContext& Context() const {
        if (context_.build_dir.empty()) {
            throw ToolError("No CMake build directory found. Start the server with --build-dir <dir> or set AETHER_BUILD_DIR.");
        }
        return context_;
    }

    // Runs `spec` with the build's compiler environment applied.
    ProcessResult Run(ProcessSpec spec) {
        spec.working_directory = spec.working_directory.empty() ? Context().build_dir.string() : spec.working_directory;
        const std::map<std::string, std::string>& base = CompilerEnvironment();
        if (!base.empty()) spec.base_environment = base;
        return RunProcess(spec);
    }

    std::string Cache(const std::string& key) const { return CacheValue(Context().build_dir / "CMakeCache.txt", key); }

    // On Windows, an MSVC build needs the Visual Studio environment: take it from the vcvars script next to the
    // compiler the cache names, once. Empty (use ours) elsewhere, or when the compiler is already reachable.
    const std::map<std::string, std::string>& CompilerEnvironment() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (env_done_) return env_;
        env_done_ = true;
#ifdef _WIN32
        const std::string cl = Cache("CMAKE_CXX_COMPILER");
        const std::size_t tools = cl.find("/VC/Tools/");
        if (tools != std::string::npos) {
            const fs::path vcvars = fs::path(cl.substr(0, tools)) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
            std::error_code ec;
            if (fs::exists(vcvars, ec)) {
                ProcessSpec spec;
                spec.command_line = "\"" + vcvars.string() + "\" >nul && set";
                spec.timeout_ms = 120000;
                const ProcessResult r = RunProcess(spec);
                if (r.started && r.exit_code == 0) {
                    for (const std::string& line : SplitLines(r.output)) {
                        const std::size_t eq = line.find('=', 1);
                        if (eq != std::string::npos) env_[line.substr(0, eq)] = line.substr(eq + 1);
                    }
                    // Keep our own variables the script doesn't know about (e.g. AETHER_*).
                    for (const auto& [k, v] : CurrentEnvironment()) env_.emplace(k, v);
                    vcvars_ = vcvars.string();
                }
            }
        }
#endif
        return env_;
    }
    const std::string& Vcvars() const { return vcvars_; }

private:
    BuildContext context_;
    std::mutex mutex_;
    bool env_done_ = false;
    std::map<std::string, std::string> env_;
    std::string vcvars_;
};

fs::path ExecutablePath(const fs::path& dir, const std::string& name) {
#ifdef _WIN32
    return dir / (name + ".exe");
#else
    return dir / name;
#endif
}

// A program inside the build directory; refuses anything that resolves outside it.
fs::path ResolveInBuild(const BuildContext& context, const std::string& program) {
    std::error_code ec;
    fs::path p = fs::path(program);
    if (p.is_relative()) p = context.build_dir / p;
#ifdef _WIN32
    if (!fs::exists(p, ec) && p.extension().empty()) p += ".exe";
#endif
    p = fs::weakly_canonical(p, ec);
    const fs::path root = fs::weakly_canonical(context.build_dir, ec);
    const std::string ps = p.generic_string(), rs = root.generic_string() + "/";
    if (ps.compare(0, rs.size(), rs) != 0) throw ToolError("\"program\" must be inside the build directory: " + context.build_dir.string());
    if (!fs::is_regular_file(p, ec)) throw ToolError("No such program in the build directory: " + program);
    return p;
}

Json ProcessJson(const ProcessResult& r, const char* what) {
    Json out = {{"ok", r.started && !r.timed_out && r.exit_code == 0},
                {"exit_code", r.exit_code},
                {"seconds", r.seconds},
                {"timed_out", r.timed_out}};
    if (!r.started) out["error"] = r.output;
    if (r.timed_out) out["error"] = std::string(what) + " timed out and was killed";
    return out;
}

} // namespace

BuildContext FindBuildContext(const std::string& explicit_dir, const fs::path& start) {
    BuildContext ctx;
    std::error_code ec;
    auto accept = [&](const fs::path& dir) {
        if (fs::exists(dir / "CMakeCache.txt", ec)) {
            ctx.build_dir = fs::weakly_canonical(dir, ec);
            ctx.source_dir = CacheValue(ctx.build_dir / "CMakeCache.txt", "CMAKE_HOME_DIRECTORY");
            return true;
        }
        return false;
    };
    if (!explicit_dir.empty()) {
        accept(explicit_dir);
        return ctx;
    }
    if (const char* env = std::getenv("AETHER_BUILD_DIR")) {
        if (*env != '\0' && accept(env)) return ctx;
    }
    for (fs::path dir = fs::weakly_canonical(start, ec); !dir.empty(); dir = dir.parent_path()) {
        if (accept(dir)) return ctx;
        if (dir == dir.parent_path()) break;
    }
    return ctx;
}

void RegisterBuildTools(McpServer& server, BuildContext context) {
    auto host = std::make_shared<BuildHost>(std::move(context));

    server.AddTool({"build_info", "The CMake build directory these tools act on: paths, generator, build type, compiler and key options.",
                    Schema(Json::object()), [host](const Json&) -> Json {
                        const BuildContext& c = host->Context();
                        Json options = Json::object();
                        std::ifstream in(c.build_dir / "CMakeCache.txt");
                        std::string line;
                        while (std::getline(in, line)) {
                            if (line.compare(0, 13, "AETHER_BUILD_") == 0 || line.compare(0, 11, "AETHER_KIT_") == 0) {
                                const std::size_t colon = line.find(':'), eq = line.find('=');
                                if (colon != std::string::npos && eq != std::string::npos) {
                                    std::string value = line.substr(eq + 1);
                                    while (!value.empty() && value.back() == '\r') value.pop_back();
                                    options[line.substr(0, colon)] = value;
                                }
                            }
                        }
                        host->CompilerEnvironment();
                        return {{"build_dir", c.build_dir.string()},
                                {"source_dir", c.source_dir.string()},
                                {"generator", host->Cache("CMAKE_GENERATOR")},
                                {"build_type", host->Cache("CMAKE_BUILD_TYPE")},
                                {"cxx_compiler", host->Cache("CMAKE_CXX_COMPILER")},
                                {"visual_studio_environment", host->Vcvars()},
                                {"options", options}};
                    }});

    server.AddTool(
        {"configure",
         "Run CMake on the build directory, optionally setting cache variables (e.g. {\"AETHER_BUILD_BENCH\": \"OFF\"}).",
         Schema({{"options", {{"type", "object"}, {"description", "Cache variables to set (-D name=value)"}}},
                 {"timeout_seconds", {{"type", "integer"}}}}),
         [host](const Json& args) -> Json {
             const BuildContext& c = host->Context();
             ProcessSpec spec;
             spec.argv = {"cmake", "-S", c.source_dir.empty() ? c.build_dir.string() : c.source_dir.string(), "-B", c.build_dir.string()};
             if (args.contains("options")) {
                 if (!args["options"].is_object()) throw ToolError("\"options\" must be an object");
                 for (auto& [name, value] : args["options"].items()) {
                     if (!std::regex_match(name, std::regex("[A-Za-z_][A-Za-z0-9_]*"))) throw ToolError("Bad option name: " + name);
                     spec.argv.push_back("-D" + name + "=" + (value.is_string() ? value.get<std::string>() : value.dump()));
                 }
             }
             spec.timeout_ms = Seconds(args, 900) * 1000;
             const ProcessResult r = host->Run(spec);
             Json out = ProcessJson(r, "configure");
             out["output_tail"] = Tail(r.output);
             return out;
         }});

    server.AddTool(
        {"build",
         "Build with `cmake --build` (all targets, or one). Returns whether it succeeded, the compiler errors found, the warning "
         "count and the tail of the output. Blocks until the build finishes or times out (default one hour).",
         Schema({{"target", {{"type", "string"}, {"description", "e.g. aether_tests, aether_player (default: all)"}}},
                 {"jobs", {{"type", "integer"}, {"description", "Parallel jobs (default: the generator's)"}}},
                 {"timeout_seconds", {{"type", "integer"}}}}),
         [host](const Json& args) -> Json {
             const BuildContext& c = host->Context();
             ProcessSpec spec;
             spec.argv = {"cmake", "--build", c.build_dir.string()};
             if (args.contains("target")) {
                 if (!args["target"].is_string() || args["target"].get<std::string>().empty() || args["target"].get<std::string>()[0] == '-') {
                     throw ToolError("\"target\" must be a target name");
                 }
                 spec.argv.push_back("--target");
                 spec.argv.push_back(args["target"].get<std::string>());
             }
             if (args.contains("jobs")) {
                 if (!args["jobs"].is_number_integer() || args["jobs"].get<long long>() < 1 || args["jobs"].get<long long>() > 256) {
                     throw ToolError("\"jobs\" must be an integer between 1 and 256");
                 }
                 spec.argv.push_back("--parallel");
                 spec.argv.push_back(std::to_string(args["jobs"].get<long long>()));
             }
             spec.timeout_ms = Seconds(args, kDefaultBuildSeconds) * 1000;
             const ProcessResult r = host->Run(spec);

             Json errors = Json::array();
             unsigned warnings = 0;
             static const std::regex error_re(R"((error\s+[A-Z]+\d+|error:|\bfatal error\b|FAILED:|undefined reference|CMake Error))");
             for (const std::string& line : SplitLines(r.output)) {
                 if (line.find("warning") != std::string::npos && line.find("warning:") != std::string::npos) ++warnings;
                 else if (line.find(": warning ") != std::string::npos) ++warnings;
                 if (errors.size() < 40 && std::regex_search(line, error_re)) errors.push_back(line);
             }
             Json out = ProcessJson(r, "build");
             out["errors"] = errors;
             out["warnings"] = warnings;
             out["output_tail"] = Tail(r.output);
             return out;
         }});

    server.AddTool({"build_targets", "The build's target names (from the generator's `help` target).",
                    Schema({{"contains", {{"type", "string"}, {"description", "Only names containing this"}}}}), [host](const Json& args) -> Json {
                        const BuildContext& c = host->Context();
                        ProcessSpec spec;
                        spec.argv = {"cmake", "--build", c.build_dir.string(), "--target", "help"};
                        spec.timeout_ms = 120000;
                        const ProcessResult r = host->Run(spec);
                        if (!r.started || r.exit_code != 0) throw ToolError("Could not list targets: " + Tail(r.output, 2000));
                        const std::string needle = args.contains("contains") && args["contains"].is_string() ? args["contains"].get<std::string>() : "";
                        Json names = Json::array();
                        for (const std::string& line : SplitLines(r.output)) {
                            const std::size_t colon = line.find(": ");
                            const std::string name = line.substr(0, colon == std::string::npos ? line.size() : colon);
                            if (name.empty() || name.find(' ') != std::string::npos || name.find('/') != std::string::npos) continue;
                            if (name.find('.') != std::string::npos) continue; // object files and such
                            if (!needle.empty() && name.find(needle) == std::string::npos) continue;
                            names.push_back(name);
                        }
                        return {{"targets", names}};
                    }});

    server.AddTool(
        {"run_tests",
         "Run the engine's unit tests (the aether_tests program in the build directory). `filter` runs only tests whose name "
         "contains one of the comma-separated pieces (AETHER_TEST_FILTER). Reports pass/fail counts and each failure with the "
         "lines printed before it. Build aether_tests first (build_first does it).",
         Schema({{"filter", {{"type", "string"}, {"description", "e.g. \"Mcp_\" or \"Physics,Audio\""}}},
                 {"build_first", {{"type", "boolean"}, {"description", "Build the aether_tests target first (default false)"}}},
                 {"timeout_seconds", {{"type", "integer"}}}}),
         [host](const Json& args) -> Json {
             const BuildContext& c = host->Context();
             Json build_result;
             if (args.contains("build_first") && args["build_first"].is_boolean() && args["build_first"].get<bool>()) {
                 ProcessSpec b;
                 b.argv = {"cmake", "--build", c.build_dir.string(), "--target", "aether_tests"};
                 b.timeout_ms = kDefaultBuildSeconds * 1000;
                 const ProcessResult br = host->Run(b);
                 if (!br.started || br.timed_out || br.exit_code != 0) {
                     Json out = ProcessJson(br, "build");
                     out["stage"] = "build";
                     out["output_tail"] = Tail(br.output);
                     return out;
                 }
             }
             const fs::path exe = ExecutablePath(c.build_dir / "tests", "aether_tests");
             std::error_code ec;
             if (!fs::exists(exe, ec)) throw ToolError("aether_tests is not built (" + exe.string() + "); pass build_first or build it");

             ProcessSpec spec;
             spec.argv = {exe.string()};
             spec.working_directory = exe.parent_path().string();
             if (args.contains("filter")) {
                 if (!args["filter"].is_string()) throw ToolError("\"filter\" must be a string");
                 spec.environment["AETHER_TEST_FILTER"] = args["filter"].get<std::string>();
             }
             spec.timeout_ms = Seconds(args, kDefaultTestSeconds) * 1000;
             const ProcessResult r = host->Run(spec);

             Json failures = Json::array();
             unsigned passed = 0, failed = 0;
             std::vector<std::string> pending;
             std::string summary;
             for (const std::string& line : SplitLines(r.output)) {
                 if (line.compare(0, 7, "[PASS] ") == 0) {
                     ++passed;
                     pending.clear();
                 } else if (line.compare(0, 7, "[FAIL] ") == 0) {
                     ++failed;
                     std::string detail;
                     for (const std::string& p : pending) detail += p + "\n";
                     failures.push_back({{"test", line.substr(7)}, {"output", Tail(detail, 3000)}});
                     pending.clear();
                 } else {
                     if (line.find(" tests passed") != std::string::npos) summary = line;
                     pending.push_back(line);
                     if (pending.size() > 40) pending.erase(pending.begin());
                 }
             }
             Json out = ProcessJson(r, "the tests");
             out["passed"] = passed;
             out["failed"] = failed;
             out["summary"] = summary;
             out["failures"] = failures;
             // A crash or hang is a failure the framework could not print.
             if (r.started && !r.timed_out && r.exit_code != 0 && failed == 0) out["output_tail"] = Tail(r.output);
             return out;
         }});

    server.AddTool(
        {"run_program",
         "Run a program from the build directory and capture its output: the headless player (aether_player --headless --frames N), "
         "the functional-test runner, the cooker, the pak tool, benchmarks. The program must be inside the build directory. "
         "No shell is involved; arguments are passed as given.",
         Schema({{"program", {{"type", "string"}, {"description", "Path relative to the build directory, e.g. player/aether_player"}}},
                 {"args", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                 {"working_directory", {{"type", "string"}, {"description", "Default: the program's folder"}}},
                 {"environment", {{"type", "object"}, {"description", "Extra environment variables"}}},
                 {"timeout_seconds", {{"type", "integer"}, {"description", "Default 120"}}}},
                {"program"}),
         [host](const Json& args) -> Json {
             const BuildContext& c = host->Context();
             if (!args["program"].is_string()) throw ToolError("\"program\" must be a string");
             const fs::path program = ResolveInBuild(c, args["program"].get<std::string>());
             ProcessSpec spec;
             spec.argv = {program.string()};
             for (std::string& a : StringArray(args, "args")) spec.argv.push_back(std::move(a));
             spec.working_directory = args.contains("working_directory") && args["working_directory"].is_string()
                                          ? args["working_directory"].get<std::string>()
                                          : program.parent_path().string();
             if (args.contains("environment")) {
                 if (!args["environment"].is_object()) throw ToolError("\"environment\" must be an object of strings");
                 for (auto& [k, v] : args["environment"].items()) {
                     if (!v.is_string()) throw ToolError("\"environment\" values must be strings");
                     spec.environment[k] = v.get<std::string>();
                 }
             }
             spec.timeout_ms = Seconds(args, kDefaultRunSeconds) * 1000;
             const ProcessResult r = host->Run(spec);
             Json out = ProcessJson(r, "the program");
             out["output"] = Tail(r.output, 20000);
             out["output_truncated"] = r.output_truncated || r.output.size() > 20000;
             return out;
         }});
}

} // namespace aether::mcp
