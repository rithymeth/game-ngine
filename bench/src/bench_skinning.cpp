// CPU skinning benchmark: serial skinning (SkinVertices, what the player always did) against its job-system version. Vertex counts: 104 is the shipped AETHER-01 Kael rig;
// the larger ones stand in for production characters. The question this answers: is CPU skinning the bottleneck at AAA
// character counts, and did the optimisation help? Each benchmark body is one frame of skinning for N characters.
#include "aether/assets/skinning.h"
#include "aether/bench/bench.h"
#include "aether/job/job_system.h"
#include "aether/math/mat4.h"
#include "aether/math/vec.h"

#include <array>
#include <cmath>
#include <memory>
#include <vector>

using namespace aether;
using namespace aether::assets;

namespace {

struct Character {
    std::vector<SkinVertex> bind;
    std::vector<SkinVertex> out;
    std::vector<std::array<u16, 4>> joints;
    std::vector<std::array<f32, 4>> weights;
    std::vector<Mat4> skin;
    std::vector<Mat4> skin_normal;
};

Character Make(usize vertices, usize joint_count) {
    Character c;
    c.bind.resize(vertices);
    c.out.resize(vertices);
    c.joints.resize(vertices);
    c.weights.resize(vertices);
    u32 s = 12345u;
    auto rnd = [&s] { s = s * 1664525u + 1013904223u; return static_cast<f32>((s >> 8) & 0xFFFF) / 65535.0f; };
    for (usize i = 0; i < vertices; ++i) {
        c.bind[i] = {{rnd(), rnd(), rnd()}, {0.0f, 1.0f, 0.0f}, {rnd(), rnd()}, {1.0f, 0.0f, 0.0f, 1.0f}};
        f32 total = 0.0f;
        for (usize k = 0; k < 4; ++k) {
            c.joints[i][k] = static_cast<u16>((i + k * 3) % joint_count);
            c.weights[i][k] = 0.1f + rnd();
            total += c.weights[i][k];
        }
        for (usize k = 0; k < 4; ++k) c.weights[i][k] /= total;
    }
    for (usize j = 0; j < joint_count; ++j) {
        const f32 a = 0.1f * static_cast<f32>(j + 1);
        Mat4 m = Mat4::Identity();
        m.cols[0] = Vec4(std::cos(a), std::sin(a), 0.0f, 0.0f);
        m.cols[1] = Vec4(-std::sin(a), std::cos(a), 0.0f, 0.0f);
        m.cols[3] = Vec4(0.01f * static_cast<f32>(j), 0.0f, 0.0f, 1.0f);
        c.skin.push_back(m);
    }
    ComputeSkinNormalMatrices(c.skin, c.skin_normal);
    return c;
}

Character& One() {
    static Character c;
    return c;
}
JobSystem& Jobs() {
    static JobSystem jobs{3};
    return jobs;
}
volatile f32 g_sink = 0.0f;

enum class Mode { Serial, Parallel };

void Run(Mode mode, usize count) {
    Character& c = One();
    for (usize i = 0; i < count; ++i) {
        // The player rebuilds the normal matrices every frame, so the benchmark does too.
        ComputeSkinNormalMatrices(c.skin, c.skin_normal);
        switch (mode) {
            case Mode::Serial:
                SkinVertices(c.bind.data(), c.out.data(), c.joints.data(), c.weights.data(), c.bind.size(), c.skin,
                             c.skin_normal);
                break;
            case Mode::Parallel:
                SkinVerticesParallel(&Jobs(), c.bind.data(), c.out.data(), c.joints.data(), c.weights.data(),
                                     c.bind.size(), c.skin, c.skin_normal);
                break;
        }
    }
    g_sink = c.out.front().position[0];
}

}  // namespace

// Names read: path / vertices x characters. "serial" is one thread; "parallel" splits each character over the job system.
AETHER_BENCH_SETUP("skinning/serial_104v_x1_aether01_kael", 200, [] { One() = Make(104, 13); }, [] { Run(Mode::Serial, 1); });
AETHER_BENCH_SETUP("skinning/serial_20k_x1_hero", 50, [] { One() = Make(20000, 90); }, [] { Run(Mode::Serial, 1); });
AETHER_BENCH_SETUP("skinning/parallel_20k_x1_hero", 50, [] { One() = Make(20000, 90); }, [] { Run(Mode::Parallel, 1); });
AETHER_BENCH_SETUP("skinning/serial_20k_x10_heroes", 10, [] { One() = Make(20000, 90); }, [] { Run(Mode::Serial, 10); });
AETHER_BENCH_SETUP("skinning/parallel_20k_x10_heroes", 10, [] { One() = Make(20000, 90); }, [] { Run(Mode::Parallel, 10); });
AETHER_BENCH_SETUP("skinning/serial_5k_x100_crowd", 5, [] { One() = Make(5000, 64); }, [] { Run(Mode::Serial, 100); });
AETHER_BENCH_SETUP("skinning/parallel_5k_x100_crowd", 5, [] { One() = Make(5000, 64); }, [] { Run(Mode::Parallel, 100); });
