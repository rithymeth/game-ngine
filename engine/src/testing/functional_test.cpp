#include "aether/testing/functional_test.h"

#include "aether/core/console.h"
#include "aether/core/log.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/serialization.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>

namespace aether {

// ---------------------------------------------------------------- context

Entity FunctionalContext::FindTagged(std::string_view tag) const {
    const std::vector<Entity> all = FindEntitiesWithTag(world, tag);
    return all.empty() ? Entity{} : all.front();
}

std::vector<Entity> FunctionalContext::AllTagged(std::string_view tag) const { return FindEntitiesWithTag(world, tag); }

Vec3 FunctionalContext::Position(Entity e) const {
    if (e.IsNull() || !world.IsAlive(e)) return Vec3();
    const Transform* t = world.GetComponent<Transform>(e);
    return t ? t->position : Vec3();
}

bool FunctionalContext::Inside(Entity e, const Vec3& c, const Vec3& h) const {
    if (e.IsNull() || !world.IsAlive(e) || !world.HasComponent<Transform>(e)) return false;
    const Vec3 p = Position(e);
    return std::fabs(p.x - c.x) <= h.x && std::fabs(p.y - c.y) <= h.y && std::fabs(p.z - c.z) <= h.z;
}

void FunctionalContext::Log(const std::string& text) {
    if (log_) log_->push_back(text);
}

void FunctionalContext::Fail(const std::string& message) {
    if (failure_.empty()) failure_ = message;
}

// ---------------------------------------------------------------- building

namespace {
std::string Fmt(f64 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}
std::string FmtVec(const Vec3& v) { return "(" + Fmt(v.x) + ", " + Fmt(v.y) + ", " + Fmt(v.z) + ")"; }
} // namespace

FunctionalTest::FunctionalTest(std::string name) : name_(std::move(name)) {}

FunctionalTest& FunctionalTest::Tag(std::string tag) {
    tags_.push_back(std::move(tag));
    return *this;
}

FunctionalTest& FunctionalTest::FixedStep(f32 dt) {
    dt_ = dt > 0.0f ? dt : dt_;
    return *this;
}

FunctionalTest& FunctionalTest::Timeout(f64 seconds) {
    timeout_ = seconds;
    return *this;
}

FunctionalTest& FunctionalTest::Scene(std::string path) {
    steps_.push_back({"load scene " + path, [path](FunctionalContext& c, FrameLoop&, f32, f64) {
                          if (LoadScene(c.world, path) || LoadSceneJson(c.world, path)) return true;
                          c.Fail("couldn't load " + path);
                          return false;
                      }});
    return *this;
}

FunctionalTest& FunctionalTest::Setup(Fn fn) { return Do("setup", std::move(fn)); }

FunctionalTest& FunctionalTest::Do(std::string description, Fn fn) {
    steps_.push_back({std::move(description), [fn](FunctionalContext& c, FrameLoop&, f32, f64) {
                          fn(c);
                          return true;
                      }});
    return *this;
}

FunctionalTest& FunctionalTest::Simulate(f64 seconds, Fn each_frame) {
    steps_.push_back({"simulate " + Fmt(seconds) + " s", [seconds, each_frame](FunctionalContext& c, FrameLoop& loop, f32 dt, f64 budget) {
                          const f64 end = c.time + seconds;
                          while (c.time + 1e-9 < end) {
                              if (c.time >= budget) {
                                  c.Fail("the test ran out of time (" + Fmt(budget) + " s simulated)");
                                  return false;
                              }
                              if (each_frame) each_frame(c);
                              loop.Tick(c.world, dt);
                              c.time += dt, ++c.frame;
                          }
                          return true;
                      }});
    return *this;
}

FunctionalTest& FunctionalTest::SimulateUntil(std::string description, Pred done, f64 timeout, Fn each_frame) {
    steps_.push_back({std::move(description), [done, timeout, each_frame](FunctionalContext& c, FrameLoop& loop, f32 dt, f64 budget) {
                          const f64 start = c.time;
                          while (!done(c)) {
                              if (c.time - start >= timeout) {
                                  c.Fail("not done after " + Fmt(timeout) + " s");
                                  return false;
                              }
                              if (c.time >= budget) {
                                  c.Fail("the test ran out of time (" + Fmt(budget) + " s simulated)");
                                  return false;
                              }
                              if (each_frame) each_frame(c);
                              loop.Tick(c.world, dt);
                              c.time += dt, ++c.frame;
                          }
                          return true;
                      }});
    return *this;
}

FunctionalTest& FunctionalTest::Check(std::string description, Pred check) {
    steps_.push_back({std::move(description), [check](FunctionalContext& c, FrameLoop&, f32, f64) {
                          if (check(c)) return true;
                          c.Fail("the check didn't hold");
                          return false;
                      }});
    return *this;
}

FunctionalTest& FunctionalTest::ExpectReaches(std::string tag, const Vec3& center, const Vec3& half, f64 timeout) {
    const std::string what = tag + " reaches " + FmtVec(center) + " +/- " + FmtVec(half) + " within " + Fmt(timeout) + " s";
    steps_.push_back({what, [tag, center, half, timeout](FunctionalContext& c, FrameLoop& loop, f32 dt, f64 budget) {
                          const f64 start = c.time;
                          while (true) {
                              const Entity e = c.FindTagged(tag);
                              if (e.IsNull()) {
                                  c.Fail("no entity is tagged " + tag);
                                  return false;
                              }
                              if (c.Inside(e, center, half)) return true;
                              if (c.time - start >= timeout || c.time >= budget) {
                                  c.Fail(tag + " is at " + FmtVec(c.Position(e)) + " after " + Fmt(c.time - start) + " s");
                                  return false;
                              }
                              loop.Tick(c.world, dt);
                              c.time += dt, ++c.frame;
                          }
                      }});
    return *this;
}

// ---------------------------------------------------------------- running

FunctionalResult FunctionalTest::Run() const {
    FunctionalResult result;
    result.name = name_;
    result.tags = tags_;
    const auto wall_start = std::chrono::steady_clock::now();
    World world;
    SystemScheduler scheduler;
    FrameLoop loop(scheduler, FixedTimestep(dt_));
    FunctionalContext context(world, scheduler);
    context.log_ = &result.log;
    // Lines can come from other threads (job workers): they're collected under a lock.
    std::mutex log_mutex;
    std::vector<std::string> logged;
    const int sink = Logger::Instance().AddSink([&](const LogLine& line) {
        std::lock_guard<std::mutex> lock(log_mutex);
        logged.push_back(std::string("[") + LogLevelName(line.level) + "] " + line.category + ": " + line.message);
    });
    for (const Step& step : steps_) {
        FunctionalStepResult sr;
        sr.description = step.description;
        context.failure_.clear();
        bool ok = false;
        try {
            ok = step.run(context, loop, dt_, timeout_);
        } catch (const std::exception& e) {
            context.Fail(std::string("threw: ") + e.what());
        } catch (...) {
            context.Fail("threw an unknown exception");
        }
        sr.passed = ok && context.failure_.empty();
        sr.message = context.failure_;
        sr.time = context.time;
        result.steps.push_back(sr);
        if (!sr.passed) {
            result.passed = false;
            result.failure = sr.description + ": " + sr.message;
            break;
        }
    }
    Logger::Instance().RemoveSink(sink);
    result.log.insert(result.log.end(), logged.begin(), logged.end());
    result.simulated = context.time;
    result.frames = context.frame;
    result.wall_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - wall_start).count();
    return result;
}

usize FunctionalReport::Passed() const {
    return static_cast<usize>(std::count_if(results.begin(), results.end(), [](const FunctionalResult& r) { return r.passed; }));
}

f64 FunctionalReport::WallMs() const {
    f64 total = 0;
    for (const FunctionalResult& r : results) total += r.wall_ms;
    return total;
}

FunctionalTestRegistry& FunctionalTestRegistry::Get() {
    static FunctionalTestRegistry registry;
    return registry;
}

void FunctionalTestRegistry::Add(std::function<FunctionalTest()> make) { makers_.push_back(std::move(make)); }

std::vector<FunctionalTest> FunctionalTestRegistry::Tests() const {
    std::vector<FunctionalTest> tests;
    for (const auto& make : makers_) tests.push_back(make());
    std::sort(tests.begin(), tests.end(), [](const FunctionalTest& a, const FunctionalTest& b) { return a.Name() < b.Name(); });
    return tests;
}

std::vector<FunctionalTest> FunctionalTestRegistry::Matching(const FunctionalRunOptions& options) const {
    std::vector<FunctionalTest> out;
    for (FunctionalTest& t : Tests()) {
        const bool name_ok = options.filters.empty() || std::any_of(options.filters.begin(), options.filters.end(), [&](const std::string& f) {
                                 return t.Name().find(f) != std::string::npos;
                             });
        const bool tag_ok = options.tags.empty() || std::any_of(options.tags.begin(), options.tags.end(), [&](const std::string& tag) {
                                return std::find(t.Tags().begin(), t.Tags().end(), tag) != t.Tags().end();
                            });
        if (name_ok && tag_ok) out.push_back(std::move(t));
    }
    return out;
}

FunctionalReport FunctionalTestRegistry::Run(const FunctionalRunOptions& options) const {
    FunctionalReport report;
    for (const FunctionalTest& t : Matching(options)) {
        report.results.push_back(t.Run());
        if (!report.results.back().passed && options.stop_on_failure) break;
    }
    return report;
}

// ---------------------------------------------------------------- reports

namespace {
std::string XmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:
            if (static_cast<unsigned char>(c) >= 0x20 || c == '\n' || c == '\t') out += c;
        }
    }
    return out;
}
} // namespace

bool WriteJUnitXml(const FunctionalReport& report, const std::string& path, const std::string& suite) {
    std::ofstream out(path);
    if (!out) return false;
    char seconds[32];
    std::snprintf(seconds, sizeof(seconds), "%.3f", report.WallMs() / 1000.0);
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<testsuites tests=\"" << report.results.size() << "\" failures=\"" << report.Failed() << "\" time=\"" << seconds << "\">\n";
    out << "  <testsuite name=\"" << XmlEscape(suite) << "\" tests=\"" << report.results.size() << "\" failures=\"" << report.Failed()
        << "\" time=\"" << seconds << "\">\n";
    for (const FunctionalResult& r : report.results) {
        std::snprintf(seconds, sizeof(seconds), "%.3f", r.wall_ms / 1000.0);
        out << "    <testcase classname=\"" << XmlEscape(suite) << "\" name=\"" << XmlEscape(r.name) << "\" time=\"" << seconds << "\">\n";
        if (!r.passed) out << "      <failure message=\"" << XmlEscape(r.failure) << "\"/>\n";
        std::string system_out;
        for (const FunctionalStepResult& s : r.steps)
            system_out += std::string(s.passed ? "ok   " : "FAIL ") + s.description + " (t=" + Fmt(s.time) + ")" +
                          (s.message.empty() ? "" : ": " + s.message) + "\n";
        for (const std::string& line : r.log) system_out += line + "\n";
        out << "      <system-out>" << XmlEscape(system_out) << "</system-out>\n";
        out << "    </testcase>\n";
    }
    out << "  </testsuite>\n</testsuites>\n";
    return static_cast<bool>(out);
}

bool WriteJsonReport(const FunctionalReport& report, const std::string& path) {
    using nlohmann::json;
    json tests = json::array();
    for (const FunctionalResult& r : report.results) {
        json steps = json::array();
        for (const FunctionalStepResult& s : r.steps)
            steps.push_back({{"description", s.description}, {"passed", s.passed}, {"message", s.message}, {"time", s.time}});
        tests.push_back({{"name", r.name}, {"tags", r.tags}, {"passed", r.passed}, {"failure", r.failure}, {"steps", steps},
                         {"simulated", r.simulated}, {"frames", r.frames}, {"wall_ms", r.wall_ms}, {"log", r.log}});
    }
    std::ofstream out(path);
    if (!out) return false;
    out << json{{"passed", report.Passed()}, {"failed", report.Failed()}, {"tests", tests}}.dump(2) << "\n";
    return static_cast<bool>(out);
}

std::string FormatFunctionalSummary(const FunctionalReport& report) {
    std::ostringstream s;
    for (const FunctionalResult& r : report.results) {
        s << (r.passed ? "[PASS] " : "[FAIL] ") << r.name << "  (" << Fmt(r.simulated) << " s simulated, " << r.frames << " frames, "
          << Fmt(r.wall_ms) << " ms)\n";
        if (!r.passed) s << "       " << r.failure << "\n";
    }
    s << report.Passed() << "/" << report.results.size() << " functional tests passed\n";
    return s.str();
}

// ---------------------------------------------------------------- the console

namespace {
AutoConsoleCommand g_functional_run(
    "functional.run", "Runs the registered functional tests: functional.run [name filter ...]",
    [](const std::vector<std::string>& args, Console& c) {
        FunctionalRunOptions options;
        options.filters = args;
        const FunctionalReport report = FunctionalTestRegistry::Get().Run(options);
        std::istringstream lines(FormatFunctionalSummary(report));
        for (std::string line; std::getline(lines, line);)
            c.Print(line, line.rfind("[FAIL]", 0) == 0 || line.rfind("       ", 0) == 0 ? LogLevel::Error : LogLevel::Info);
    });
AutoConsoleCommand g_functional_list("functional.list", "Lists the registered functional tests",
                                     [](const std::vector<std::string>&, Console& c) {
                                         for (const FunctionalTest& t : FunctionalTestRegistry::Get().Tests()) {
                                             std::string tags;
                                             for (const std::string& tag : t.Tags()) tags += " #" + tag;
                                             c.Print(t.Name() + tags);
                                         }
                                     });
} // namespace

} // namespace aether
