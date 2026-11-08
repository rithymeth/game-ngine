#include "aether/assets/skinning.h"

#include "aether/job/job_system.h"

#include <algorithm>
#include <cmath>

namespace aether::assets {

void ComputeSkinNormalMatrices(std::span<const Mat4> skin, std::vector<Mat4>& out) {
    out.clear();
    out.reserve(skin.size());
    for (const Mat4& m : skin) {
        Mat4 inverse;
        if (!m.TryInverse(inverse)) {
            out.push_back(Mat4::Identity());
            continue;
        }
        Mat4 n;
        n.cols[0] = Vec4(inverse.cols[0].x, inverse.cols[1].x, inverse.cols[2].x, 0.0f);
        n.cols[1] = Vec4(inverse.cols[0].y, inverse.cols[1].y, inverse.cols[2].y, 0.0f);
        n.cols[2] = Vec4(inverse.cols[0].z, inverse.cols[1].z, inverse.cols[2].z, 0.0f);
        n.cols[3] = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
        out.push_back(n);
    }
}

void SkinVertices(const SkinVertex* bind, SkinVertex* out, const std::array<u16, 4>* joints,
                  const std::array<f32, 4>* weights, usize count, std::span<const Mat4> skin,
                  std::span<const Mat4> skin_normal) {
    std::copy(bind, bind + count, out);
    for (usize v = 0; v < count; ++v) {
        const Vec4 position{bind[v].position[0], bind[v].position[1], bind[v].position[2], 1.0f};
        const Vec4 normal{bind[v].normal[0], bind[v].normal[1], bind[v].normal[2], 0.0f};
        const Vec4 tangent{bind[v].tangent[0], bind[v].tangent[1], bind[v].tangent[2], 0.0f};
        f32 x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0, tx = 0, ty = 0, tz = 0, total = 0;
        for (usize k = 0; k < 4; ++k) {
            const usize joint = joints[v][k];
            const f32 w = weights[v][k];
            if (w <= 0.0f || joint >= skin.size() || joint >= skin_normal.size()) continue;
            const Vec4 p = skin[joint] * position;
            const Vec4 n = skin_normal[joint] * normal;
            const Vec4 t = skin[joint] * tangent;
            x += p.x * w; y += p.y * w; z += p.z * w;
            nx += n.x * w; ny += n.y * w; nz += n.z * w;
            tx += t.x * w; ty += t.y * w; tz += t.z * w;
            total += w;
        }
        if (total <= 0.0f) continue;
        out[v].position[0] = x / total;
        out[v].position[1] = y / total;
        out[v].position[2] = z / total;
        const Vec3 sn{nx / total, ny / total, nz / total};
        if (sn.LengthSq() > 1e-12f) {
            const Vec3 nn = sn.Normalized();
            out[v].normal[0] = nn.x;
            out[v].normal[1] = nn.y;
            out[v].normal[2] = nn.z;
        }
        Vec3 st{tx / total, ty / total, tz / total};
        const Vec3 normal_for_tangent{out[v].normal[0], out[v].normal[1], out[v].normal[2]};
        st = st - normal_for_tangent * normal_for_tangent.Dot(st);
        if (st.LengthSq() > 1e-12f) {
            st = st.Normalized();
            out[v].tangent[0] = st.x;
            out[v].tangent[1] = st.y;
            out[v].tangent[2] = st.z;
        }
    }
}

namespace {

struct Chunk {
    const SkinVertex* bind;
    SkinVertex* out;
    const std::array<u16, 4>* joints;
    const std::array<f32, 4>* weights;
    usize count;
    std::span<const Mat4> skin;
    std::span<const Mat4> skin_normal;
};

void RunChunk(void* data) {
    const Chunk& c = *static_cast<const Chunk*>(data);
    SkinVertices(c.bind, c.out, c.joints, c.weights, c.count, c.skin, c.skin_normal);
}

}  // namespace

void SkinVerticesParallel(JobSystem* jobs, const SkinVertex* bind, SkinVertex* out, const std::array<u16, 4>* joints,
                          const std::array<f32, 4>* weights, usize count, std::span<const Mat4> skin,
                          std::span<const Mat4> skin_normal, usize min_chunk) {
    constexpr usize kMaxChunks = 32;
    const usize chunks = jobs ? std::min<usize>(kMaxChunks, count / std::max<usize>(min_chunk, 1)) : 0;
    if (chunks < 2) {
        SkinVertices(bind, out, joints, weights, count, skin, skin_normal);
        return;
    }
    std::array<Chunk, kMaxChunks> parts;
    std::array<JobDecl, kMaxChunks> decls;
    const usize per = (count + chunks - 1) / chunks;
    usize used = 0;
    for (usize start = 0; start < count; start += per, ++used) {
        const usize n = std::min(per, count - start);
        parts[used] = {bind + start, out + start, joints + start, weights + start, n, skin, skin_normal};
        decls[used] = {&RunChunk, &parts[used]};
    }
    JobCounter counter{0};
    jobs->ScheduleBatch(decls.data(), static_cast<u32>(used), counter);
    jobs->Wait(counter);
}

}  // namespace aether::assets
