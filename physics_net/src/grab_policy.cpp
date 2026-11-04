#include "aether/physics_net/grab_policy.h"

#include <algorithm>
#include <cmath>

namespace aether::physics_net {
namespace {

f32 Distance(const Vec3& a, const Vec3& b) {
    const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

GrabStatus ValidateGrab(const GrabPolicy& policy, const GrabFacts& facts) {
    if (!facts.player_known) return GrabStatus::UnknownPlayer;
    if (!facts.rate_ok) return GrabStatus::RateLimited;
    if (!facts.object_known) return GrabStatus::UnknownObject;
    if (!facts.grabbable) return GrabStatus::NotGrabbable;
    if (facts.already_holding_object || facts.peer_grabs >= policy.max_grabs_per_peer) return GrabStatus::AlreadyHolding;
    if (Distance(facts.holder_position, facts.point) > policy.reach) return GrabStatus::TooFar;
    if (Distance(facts.object_position, facts.point) > policy.max_object_offset) return GrabStatus::TooFar;
    if (facts.object_holders >= policy.max_holders_per_object) return GrabStatus::TooManyHolders;
    return GrabStatus::Granted;
}

Vec3 ClampGrabTarget(const GrabPolicy& policy, const Vec3& holder, const Vec3& target) {
    const f32 distance = Distance(holder, target);
    if (!(distance > policy.max_target_distance) || distance <= 0.0f) return target;
    const f32 scale = policy.max_target_distance / distance;
    return Vec3(holder.x + (target.x - holder.x) * scale, holder.y + (target.y - holder.y) * scale, holder.z + (target.z - holder.z) * scale);
}

bool TokenBucket::Take(f64 now, f64 per_second, f64 burst) {
    if (per_second <= 0.0) return true; // off
    if (tokens < 0.0) {
        tokens = burst;
        last = now;
    }
    tokens = std::min(burst, tokens + std::max(0.0, now - last) * per_second);
    last = now;
    if (tokens < 1.0) return false;
    tokens -= 1.0;
    return true;
}

} // namespace aether::physics_net
