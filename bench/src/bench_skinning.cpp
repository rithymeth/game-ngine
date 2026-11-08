// CPU skinning benchmark. The loop mirrors player/src/scene_renderer.cpp (position, normal and tangent blended
// over four influences, then normalised and the tangent re-orthogonalised) so the numbers describe what the player
// does today. Vertex counts: 104 and 112 are the shipped AETHER-01 Kael and Warden rigs; the larger ones stand in for
// production characters. The question this answers: is CPU skinning the bottleneck at AAA character counts?
#include "aether/bench/bench.h"
#include "aether/math/mat4.h"
#include "aether/math/vec.h"

#include <array>
#include <cmath>
#include <vector>

using namespace aether;

namespace {

struct Vertex {
    f32 position[3];
    f32 normal[3];
    f32 uv[2];
    f32 tangent[4];
};

struct Character {
    std::vector<Vertex> bind;
    std::vector<Vertex> out;
    std::vector<std::array<u16, 4>> joints;
    std::vector<std::array<f32, 4>> weights;
    std::vector<Mat4> skin;
    std::vector<Mat4> skin_normal;
};

Mat4 NormalOf(const Mat4& m) {
    Mat4 inverse;
    if (!m.TryInverse(inverse)) return Mat4::Identity();
    Mat4 n;
    n.cols[0] = Vec4(inverse.cols[0].x, inverse.cols[1].x, inverse.cols[2].x, 0.0f);
    n.cols[1] = Vec4(inverse.cols[0].y, inverse.cols[1].y, inverse.cols[2].y, 0.0f);
    n.cols[2] = Vec4(inverse.cols[0].z, inverse.cols[1].z, inverse.cols[2].z, 0.0f);
    n.cols[3] = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return n;
}

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
    c.skin.resize(joint_count);
    for (usize j = 0; j < joint_count; ++j) {
        const f32 a = 0.1f * static_cast<f32>(j + 1);
        Mat4 m = Mat4::Identity();
        m.cols[0] = Vec4(std::cos(a), std::sin(a), 0.0f, 0.0f);
        m.cols[1] = Vec4(-std::sin(a), std::cos(a), 0.0f, 0.0f);
        m.cols[3] = Vec4(0.01f * static_cast<f32>(j), 0.0f, 0.0f, 1.0f);
        c.skin[j] = m;
    }
    return c;
}

// One character, one frame: what the player does per skinned node.
void Skin(Character& c) {
    c.skin_normal.clear();
    c.skin_normal.reserve(c.skin.size());
    for (const Mat4& m : c.skin) c.skin_normal.push_back(NormalOf(m));
    std::copy(c.bind.begin(), c.bind.end(), c.out.begin());
    for (usize v = 0; v < c.out.size(); ++v) {
        const auto& joints = c.joints[v];
        const auto& weights = c.weights[v];
        const Vec4 position{c.bind[v].position[0], c.bind[v].position[1], c.bind[v].position[2], 1.0f};
        const Vec4 normal{c.bind[v].normal[0], c.bind[v].normal[1], c.bind[v].normal[2], 0.0f};
        const Vec4 tangent{c.bind[v].tangent[0], c.bind[v].tangent[1], c.bind[v].tangent[2], 0.0f};
        f32 x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0, tx = 0, ty = 0, tz = 0, total = 0;
        for (usize k = 0; k < 4; ++k) {
            const usize joint = joints[k];
            const f32 w = weights[k];
            if (w <= 0.0f || joint >= c.skin.size()) continue;
            const Vec4 p = c.skin[joint] * position;
            const Vec4 n = c.skin_normal[joint] * normal;
            const Vec4 t = c.skin[joint] * tangent;
            x += p.x * w; y += p.y * w; z += p.z * w;
            nx += n.x * w; ny += n.y * w; nz += n.z * w;
            tx += t.x * w; ty += t.y * w; tz += t.z * w;
            total += w;
        }
        if (total <= 0.0f) continue;
        c.out[v].position[0] = x / total; c.out[v].position[1] = y / total; c.out[v].position[2] = z / total;
        const Vec3 sn{nx / total, ny / total, nz / total};
        if (sn.LengthSq() > 1e-12f) {
            const Vec3 nn = sn.Normalized();
            c.out[v].normal[0] = nn.x; c.out[v].normal[1] = nn.y; c.out[v].normal[2] = nn.z;
        }
        Vec3 st{tx / total, ty / total, tz / total};
        const Vec3 nft{c.out[v].normal[0], c.out[v].normal[1], c.out[v].normal[2]};
        st = st - nft * nft.Dot(st);
        if (st.LengthSq() > 1e-12f) {
            st = st.Normalized();
            c.out[v].tangent[0] = st.x; c.out[v].tangent[1] = st.y; c.out[v].tangent[2] = st.z;
        }
    }
}

struct Crowd {
    std::vector<Character> characters;
};
Crowd& State() {
    static Crowd s;
    return s;
}
volatile f32 g_sink = 0.0f;

void Setup(usize vertices, usize joints, usize count) {
    State().characters.clear();
    State().characters.push_back(Make(vertices, joints));
    // Every instance runs the same loop; one set of buffers is enough for timing, count repeats the work.
    (void)count;
}
void Run(usize count) {
    Character& c = State().characters.front();
    for (usize i = 0; i < count; ++i) Skin(c);
    g_sink = c.out.front().position[0];
}

}  // namespace

// One frame of skinning for N characters. Names read: vertices x characters.
AETHER_BENCH_SETUP("skinning/cpu_104v_x1_aether01_kael", 200, [] { Setup(104, 13, 1); }, [] { Run(1); });
AETHER_BENCH_SETUP("skinning/cpu_5k_x10_npc_group", 20, [] { Setup(5000, 64, 10); }, [] { Run(10); });
AETHER_BENCH_SETUP("skinning/cpu_20k_x1_hero", 50, [] { Setup(20000, 90, 1); }, [] { Run(1); });
AETHER_BENCH_SETUP("skinning/cpu_20k_x10_heroes", 10, [] { Setup(20000, 90, 10); }, [] { Run(10); });
AETHER_BENCH_SETUP("skinning/cpu_5k_x100_crowd", 5, [] { Setup(5000, 64, 100); }, [] { Run(100); });
