// aether_bp_bench: the Phase 12 Blueprint VM benchmark (ROADMAP.md §12.6,
// "10,000 BP instances ticking a 20-node graph costs under 2 ms").
//
//   aether_bp_bench [path/to/BP_Spinner.abp] [--count N] [--frames N] [--check MS]
//
// Loads the Blueprint (the repo's assets/blueprints/BP_Spinner.abp by
// default), attaches it to N entities, and times BlueprintVM::Tick. With
// --check, exits with 1 if a frame averages more than MS milliseconds.
#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/ecs/world.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace aether;

int main(int argc, char** argv) {
    std::string path = AETHER_BENCH_DEFAULT_BLUEPRINT;
    int count = 10'000, frames = 120;
    double check = 0.0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--count") && i + 1 < argc) count = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--check") && i + 1 < argc) check = std::atof(argv[++i]);
        else path = argv[i];
    }
    bp::Blueprint blueprint;
    std::string error;
    if (!bp::LoadBlueprint(path, blueprint, &error)) {
        std::fprintf(stderr, "can't load %s: %s\n", path.c_str(), error.c_str());
        return 2;
    }
    const bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
    if (!compiled.Ok()) {
        for (const bp::Diagnostic& d : compiled.diagnostics.diagnostics) std::fprintf(stderr, "%s %s\n", d.code.c_str(), d.message.c_str());
        return 2;
    }
    usize nodes = 0;
    for (const bp::Graph& g : blueprint.graphs) nodes += g.nodes.size();

    World world;
    bp::BlueprintVM vm(world);
    for (int i = 0; i < count; ++i) vm.Attach(world.CreateEntity(Transform{}), compiled.blueprint);
    vm.BeginPlay();
    vm.Tick(1.0f / 60.0f); // warm up

    std::vector<double> times;
    times.reserve(static_cast<usize>(frames));
    for (int f = 0; f < frames; ++f) {
        const auto start = std::chrono::steady_clock::now();
        vm.Tick(1.0f / 60.0f);
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(times.begin(), times.end());
    double total = 0.0;
    for (double t : times) total += t;
    const double mean = total / static_cast<double>(frames);
    std::printf("%d instances of %s (%zu nodes), %d frames: mean %.3f ms, median %.3f ms, p95 %.3f ms per frame\n", count,
                path.substr(path.find_last_of("/\\") + 1).c_str(), nodes, frames, mean, times[times.size() / 2],
                times[std::min(times.size() - 1, times.size() * 95 / 100)]);
    if (!vm.Errors().empty()) {
        std::fprintf(stderr, "runtime errors: %s\n", vm.Errors().front().message.c_str());
        return 2;
    }
    if (check > 0.0 && mean > check) {
        std::printf("FAIL: over the %.3f ms budget\n", check);
        return 1;
    }
    return 0;
}
