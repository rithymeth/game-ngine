#include "vfx/vfx_preview.h"

#include "aether/math/math.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace aether::editor {

using namespace vfx;

void ParticlePreview::Reset(const ParticleSystemAsset& asset) {
    asset_ = asset;
    instance_ = std::make_unique<ParticleSystemInstance>(asset_, seed_);
    instance_->Teleport({{0, 0, 0}, {}});
    time_ = carry_ = 0.0f;
    ms_.assign(asset_.emitters.size(), 0.0);
}

void ParticlePreview::Advance(f32 dt) {
    if (!instance_) return;
    carry_ += std::max(dt, 0.0f);
    while (carry_ >= kStep - 1e-6f) {
        carry_ -= kStep;
        const auto t0 = std::chrono::steady_clock::now();
        instance_->Update(kStep);
        const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
        // The step's time shared out by particle count (without timing each emitter apart).
        const f64 total = static_cast<f64>(std::max<usize>(instance_->Count(), 1));
        for (usize i = 0; i < ms_.size() && i < instance_->EmitterCount(); ++i) ms_[i] = ms * static_cast<f64>(instance_->Emitter(i).Count()) / total;
        time_ += kStep;
    }
}

void ParticlePreview::Seek(const ParticleSystemAsset& asset, f32 time) {
    Reset(asset);
    const f32 target = std::clamp(time, 0.0f, kMaxSeek);
    const long steps = std::lround(target / kStep);
    for (long i = 0; i < steps; ++i) Advance(kStep);
}

std::vector<ParticlePreview::EmitterStats> ParticlePreview::Stats() const {
    std::vector<EmitterStats> out;
    if (!instance_) return out;
    for (usize i = 0; i < instance_->EmitterCount(); ++i) {
        const EmitterInstance& e = instance_->Emitter(i);
        EmitterStats s;
        s.name = e.Asset().settings.name;
        s.count = e.Count();
        s.spawned = e.TotalSpawned();
        s.bounds = e.ComputeBounds();
        s.update_ms = i < ms_.size() ? ms_[i] : 0.0;
        s.gpu_supported = CheckGpuSupport(e.Asset()).supported;
        s.target = ChooseSimTarget(e.Asset(), true);
        out.push_back(std::move(s));
    }
    return out;
}

ParticleCamera ParticlePreview::Camera() const {
    const Vec3 eye = target + Vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)) * distance;
    return ParticleCamera::FromView(Mat4::LookAtRH(eye, target, {0, 1, 0}));
}

Mat4 ParticlePreview::ViewProjection(f32 aspect) const {
    const Vec3 eye = target + Vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)) * distance;
    return Mat4::PerspectiveRH(Radians(50.0f), std::max(aspect, 0.01f), 0.05f, 1000.0f) * Mat4::LookAtRH(eye, target, {0, 1, 0});
}

bool ParticlePreview::Project(const Vec3& p, f32 w, f32 h, f32& x, f32& y, f32& depth) const {
    const Vec4 c = ViewProjection(w / std::max(h, 1.0f)) * Vec4(p.x, p.y, p.z, 1.0f);
    if (c.w <= 1e-4f) return false;
    x = (c.x / c.w * 0.5f + 0.5f) * w;
    y = (0.5f - c.y / c.w * 0.5f) * h;
    depth = c.w;
    return true;
}

} // namespace aether::editor
