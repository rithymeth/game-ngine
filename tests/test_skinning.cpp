#include "../player/src/skin_workers.h"
#include "aether/assets/skinning.h"
#include "aether/job/job_system.h"
#include "test_framework.h"

#include <cmath>
#include <vector>

using namespace aether;
using namespace aether::assets;

// SkinVertices must match an independent statement of the maths (Oracle, below) to float rounding, including
// the edge cases the loop handled: zero weights, joints out of range, and vertices with no valid influence at all.

namespace {

struct Mesh {
    std::vector<SkinVertex> bind;
    std::vector<std::array<u16, 4>> joints;
    std::vector<std::array<f32, 4>> weights;
    std::vector<Mat4> skin;
    std::vector<Mat4> skin_normal;
};

Mesh MakeMesh(usize vertices, usize joint_count) {
    Mesh m;
    u32 s = 987654321u;
    auto rnd = [&s] {
        s = s * 1664525u + 1013904223u;
        return static_cast<f32>((s >> 8) & 0xFFFF) / 65535.0f;
    };
    m.bind.resize(vertices);
    m.joints.resize(vertices);
    m.weights.resize(vertices);
    for (usize i = 0; i < vertices; ++i) {
        const Vec3 n = Vec3(rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f).Normalized();
        const Vec3 t = Vec3(rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f).Normalized();
        m.bind[i] = {{rnd(), rnd(), rnd()}, {n.x, n.y, n.z}, {rnd(), rnd()}, {t.x, t.y, t.z, 1.0f}};
        f32 total = 0.0f;
        for (usize k = 0; k < 4; ++k) {
            m.joints[i][k] = static_cast<u16>((i * 7 + k * 3) % joint_count);
            m.weights[i][k] = 0.05f + rnd();
            total += m.weights[i][k];
        }
        for (usize k = 0; k < 4; ++k) m.weights[i][k] /= total;
    }
    for (usize j = 0; j < joint_count; ++j) {
        const f32 a = 0.37f * static_cast<f32>(j + 1);
        Mat4 r = Mat4::Identity();
        r.cols[0] = Vec4(std::cos(a), std::sin(a), 0.0f, 0.0f);
        r.cols[1] = Vec4(-std::sin(a), std::cos(a), 0.0f, 0.0f);
        const f32 sc = 0.8f + 0.1f * static_cast<f32>(j % 5);  // non-uniform scale in a column so the normal matrix matters
        r.cols[2] = Vec4(0.0f, 0.0f, sc, 0.0f);
        r.cols[3] = Vec4(0.1f * static_cast<f32>(j), -0.05f * static_cast<f32>(j), 0.02f, 1.0f);
        m.skin.push_back(r);
    }
    ComputeSkinNormalMatrices(m.skin, m.skin_normal);
    return m;
}

f32 MaxDifference(const std::vector<SkinVertex>& a, const std::vector<SkinVertex>& b) {
    f32 worst = 0.0f;
    for (usize i = 0; i < a.size(); ++i) {
        const f32* pa = &a[i].position[0];
        const f32* pb = &b[i].position[0];
        for (usize k = 0; k < 12; ++k) worst = std::max(worst, std::fabs(pa[k] - pb[k]));
    }
    return worst;
}


// An independent statement of what skinning must produce: each influence transforms position, normal and tangent, the
// results are weighted and divided by the total valid weight, the normal is renormalised and the tangent is made
// orthogonal to it. Written out per vertex with plain math so a future faster SkinVertices (SIMD, GPU readback) has
// something other than itself to be compared against.
void Oracle(const Mesh& m, std::vector<SkinVertex>& out) {
    out = m.bind;
    for (usize v = 0; v < m.bind.size(); ++v) {
        Vec3 p{0, 0, 0}, n{0, 0, 0}, t{0, 0, 0};
        f32 total = 0.0f;
        for (usize k = 0; k < 4; ++k) {
            const usize j = m.joints[v][k];
            const f32 w = m.weights[v][k];
            if (w <= 0.0f || j >= m.skin.size()) continue;
            const Vec4 pp = m.skin[j] * Vec4(m.bind[v].position[0], m.bind[v].position[1], m.bind[v].position[2], 1.0f);
            const Vec4 nn = m.skin_normal[j] * Vec4(m.bind[v].normal[0], m.bind[v].normal[1], m.bind[v].normal[2], 0.0f);
            const Vec4 tt = m.skin[j] * Vec4(m.bind[v].tangent[0], m.bind[v].tangent[1], m.bind[v].tangent[2], 0.0f);
            p = p + Vec3(pp.x, pp.y, pp.z) * w;
            n = n + Vec3(nn.x, nn.y, nn.z) * w;
            t = t + Vec3(tt.x, tt.y, tt.z) * w;
            total += w;
        }
        if (total <= 0.0f) continue;
        p = p * (1.0f / total);
        n = n * (1.0f / total);
        t = t * (1.0f / total);
        out[v].position[0] = p.x; out[v].position[1] = p.y; out[v].position[2] = p.z;
        if (n.LengthSq() > 1e-12f) {
            n = n.Normalized();
            out[v].normal[0] = n.x; out[v].normal[1] = n.y; out[v].normal[2] = n.z;
        }
        const Vec3 on{out[v].normal[0], out[v].normal[1], out[v].normal[2]};
        t = t - on * on.Dot(t);
        if (t.LengthSq() > 1e-12f) {
            t = t.Normalized();
            out[v].tangent[0] = t.x; out[v].tangent[1] = t.y; out[v].tangent[2] = t.z;
        }
    }
}

}  // namespace

AETHER_TEST(Skinning_MatchesOracle) {
    Mesh m = MakeMesh(3000, 40);
    std::vector<SkinVertex> fast(m.bind.size()), ref;
    SkinVertices(m.bind.data(), fast.data(), m.joints.data(), m.weights.data(), m.bind.size(), m.skin, m.skin_normal);
    Oracle(m, ref);
    AETHER_CHECK(MaxDifference(fast, ref) < 1e-4f);
    // Skinning actually moved something, so the comparison above is not trivially equal bind poses.
    AETHER_CHECK(MaxDifference(fast, m.bind) > 0.05f);
}

AETHER_TEST(Skinning_EdgeCasesMatchOracle) {
    Mesh m = MakeMesh(8, 4);
    m.weights[0] = {0.0f, 0.0f, 0.0f, 0.0f};          // no weight at all: keeps the bind pose
    m.joints[1] = {200, 201, 202, 203};               // every joint out of range: keeps the bind pose
    m.joints[2] = {1, 250, 2, 250};                   // some out of range: the valid ones are renormalised
    m.weights[3] = {1.0f, 0.0f, 0.0f, 0.0f};          // one joint only
    m.weights[4] = {0.5f, -0.2f, 0.5f, 0.0f};         // negative weight is ignored like zero
    std::vector<SkinVertex> fast(8), ref;
    SkinVertices(m.bind.data(), fast.data(), m.joints.data(), m.weights.data(), 8, m.skin, m.skin_normal);
    Oracle(m, ref);
    AETHER_CHECK(MaxDifference(fast, ref) < 1e-4f);
    AETHER_CHECK(MaxDifference({fast[0]}, {m.bind[0]}) == 0.0f);
    AETHER_CHECK(MaxDifference({fast[1]}, {m.bind[1]}) == 0.0f);
}

AETHER_TEST(Skinning_ParallelMatchesSerial) {
    Mesh m = MakeMesh(20000, 64);
    std::vector<SkinVertex> serial(m.bind.size()), parallel(m.bind.size());
    SkinVertices(m.bind.data(), serial.data(), m.joints.data(), m.weights.data(), m.bind.size(), m.skin, m.skin_normal);
    JobSystem jobs{2};
    SkinVerticesParallel(&jobs, m.bind.data(), parallel.data(), m.joints.data(), m.weights.data(), m.bind.size(), m.skin,
                         m.skin_normal, 1024);
    // Same arithmetic per vertex, only split differently: bit-identical.
    AETHER_CHECK(MaxDifference(serial, parallel) == 0.0f);
    // A null job system, or a mesh smaller than two chunks, falls back to the serial path.
    std::vector<SkinVertex> fallback(m.bind.size());
    SkinVerticesParallel(nullptr, m.bind.data(), fallback.data(), m.joints.data(), m.weights.data(), m.bind.size(),
                         m.skin, m.skin_normal);
    AETHER_CHECK(MaxDifference(serial, fallback) == 0.0f);
}

// The skinning is the same arithmetic per vertex whatever the split, so every worker count gives the bytes the serial loop does, on a vertex
// count that does not divide into the chunks evenly.
AETHER_TEST(Skinning_ParallelIsBitIdenticalAtEveryWorkerCount) {
    Mesh m = MakeMesh(20011, 64);
    std::vector<SkinVertex> serial(m.bind.size());
    SkinVertices(m.bind.data(), serial.data(), m.joints.data(), m.weights.data(), m.bind.size(), m.skin, m.skin_normal);
    for (const u32 workers : {1u, 2u, 3u, 5u, 7u, 15u}) {
        JobSystem jobs{workers};
        std::vector<SkinVertex> parallel(m.bind.size());
        SkinVerticesParallel(&jobs, m.bind.data(), parallel.data(), m.joints.data(), m.weights.data(), m.bind.size(), m.skin, m.skin_normal, 1024);
        AETHER_CHECK(MaxDifference(serial, parallel) == 0.0f);
    }
}

// How many workers the player gives the skinner: scaled to the machine, bounded, overridable.
AETHER_TEST(Skinning_WorkerCountScalesWithTheMachine) {
    using player::SkinWorkerCount;
    AETHER_CHECK(SkinWorkerCount(0) == 2);                                   // unknown: the old default
    AETHER_CHECK(SkinWorkerCount(1) == 0 && SkinWorkerCount(2) == 0);        // too small to split across
    AETHER_CHECK(SkinWorkerCount(3) == 1 && SkinWorkerCount(4) == 1);
    AETHER_CHECK(SkinWorkerCount(6) == 2 && SkinWorkerCount(8) == 3 && SkinWorkerCount(12) == 5);
    AETHER_CHECK(SkinWorkerCount(16) == 7);
    AETHER_CHECK(SkinWorkerCount(32) == 7 && SkinWorkerCount(128) == 7);     // the speed-up flattens: capped
    // The override: a number wins, including 0 (render thread alone) and a cap of 32; anything else is ignored.
    AETHER_CHECK(SkinWorkerCount(16, "3") == 3 && SkinWorkerCount(16, "0") == 0 && SkinWorkerCount(2, "5") == 5);
    AETHER_CHECK(SkinWorkerCount(16, "1000") == 32);
    AETHER_CHECK(SkinWorkerCount(16, "") == 7 && SkinWorkerCount(16, "many") == 7 && SkinWorkerCount(16, "-2") == 7 && SkinWorkerCount(16, "4x") == 7);
}
