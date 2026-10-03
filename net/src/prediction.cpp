#include "aether/net/prediction.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aether::net {

namespace {

constexpr u8 kInputs = 4, kAck = 5;

void PutU32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}
void PutF32(std::vector<u8>& out, f32 v) {
    u32 bits;
    std::memcpy(&bits, &v, 4);
    PutU32(out, bits);
}

struct In {
    std::span<const u8> data;
    usize at = 1;
    bool ok = true;
    u8 U8() { return (ok = ok && at + 1 <= data.size()) ? data[at++] : 0; }
    u32 U32() {
        if (!(ok = ok && at + 4 <= data.size())) return 0;
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<u32>(data[at++]) << (8 * i);
        return v;
    }
    f32 F32() {
        const u32 bits = U32();
        f32 v;
        std::memcpy(&v, &bits, 4);
        return v;
    }
};

MovementStep DefaultStep() {
    return [](World&, Entity, Transform& t, NetMovement& m, const MovementInput& in) { StepMovement(t, m, in); };
}

} // namespace

void StepMovement(Transform& transform, NetMovement& movement, const MovementInput& input) {
    const f32 dt = input.dt;
    // Horizontal velocity eases towards the input's.
    const f32 tx = input.move_x * movement.speed, tz = input.move_z * movement.speed;
    const f32 dx = tx - movement.velocity.x, dz = tz - movement.velocity.z;
    const f32 dist = std::sqrt(dx * dx + dz * dz), step = movement.acceleration * dt;
    if (dist <= step || dist <= 0.0f) movement.velocity.x = tx, movement.velocity.z = tz;
    else movement.velocity.x += dx / dist * step, movement.velocity.z += dz / dist * step;
    // Jumping and gravity.
    if (movement.grounded && input.jump) movement.velocity.y = movement.jump_speed, movement.grounded = false;
    if (!movement.grounded) movement.velocity.y += movement.gravity * dt;
    transform.position = transform.position + movement.velocity * dt;
    if (transform.position.y <= movement.ground_height && movement.velocity.y <= 0.0f) {
        transform.position.y = movement.ground_height;
        movement.velocity.y = 0.0f;
        movement.grounded = true;
    }
}

MovementInput SanitizeInput(MovementInput input, f32 max_dt) {
    if (!std::isfinite(input.dt) || input.dt <= 0.0f) input.dt = 0.0f;
    input.dt = std::min(input.dt, max_dt);
    if (!std::isfinite(input.move_x) || !std::isfinite(input.move_z)) input.move_x = input.move_z = 0.0f;
    const f32 len = std::sqrt(input.move_x * input.move_x + input.move_z * input.move_z);
    if (len > 1.0f) input.move_x /= len, input.move_z /= len;
    return input;
}

// ---------------------------------------------------------------- server

MovementServer::MovementServer(World& world, NetHost& host, ReplicationServer& replication, PredictionConfig config)
    : world_(world), host_(host), replication_(replication), config_(config), step_(DefaultStep()) {}

u32 MovementServer::LastProcessed(u32 net_id) const {
    auto it = characters_.find(net_id);
    return it == characters_.end() ? 0 : it->second.last;
}

bool MovementServer::HandleEvent(const NetEvent& event) {
    if (event.type != NetEventType::Message || event.channel != config_.input_channel || event.data.empty() ||
        event.data[0] != kInputs)
        return false;
    In in{event.data};
    const u32 net_id = in.U32();
    const u8 count = in.U8();
    std::vector<MovementInput> inputs(count);
    for (MovementInput& i : inputs) {
        i.sequence = in.U32();
        i.dt = in.F32();
        i.move_x = in.F32();
        i.move_z = in.F32();
        i.jump = in.U8() != 0;
    }
    if (!in.ok) {
        ++stats_.malformed;
        return true;
    }
    const Entity entity = replication_.FindEntity(net_id);
    const NetIdentity* ni = entity.IsNull() ? nullptr : world_.GetComponent<NetIdentity>(entity);
    if (!ni || ni->owner != event.peer || !world_.HasComponent<NetMovement>(entity)) {
        ++stats_.rejected;
        return true;
    }
    Character& c = characters_[net_id];
    for (const MovementInput& i : inputs) {
        if (i.sequence <= c.last) {
            ++stats_.duplicates;
            continue;
        }
        auto at = std::lower_bound(c.queue.begin(), c.queue.end(), i.sequence,
                                   [](const MovementInput& q, u32 s) { return q.sequence < s; });
        if (at != c.queue.end() && at->sequence == i.sequence) {
            ++stats_.duplicates;
            continue;
        }
        c.queue.insert(at, i);
    }
    while (c.queue.size() > config_.max_pending) c.queue.pop_back(); // far-future floods
    return true;
}

void MovementServer::Update(f64) {
    for (auto it = characters_.begin(); it != characters_.end();) {
        const Entity entity = replication_.FindEntity(it->first);
        if (entity.IsNull() || !world_.IsAlive(entity)) {
            it = characters_.erase(it);
            continue;
        }
        Character& c = it->second;
        Transform* t = world_.GetComponent<Transform>(entity);
        NetMovement* m = world_.GetComponent<NetMovement>(entity);
        const NetIdentity* ni = world_.GetComponent<NetIdentity>(entity);
        if (t && m && ni) {
            for (usize n = 0; n < config_.max_inputs_per_update && !c.queue.empty(); ++n) {
                const MovementInput input = SanitizeInput(c.queue.front(), config_.max_dt);
                c.queue.pop_front();
                step_(world_, entity, *t, *m, input);
                c.last = input.sequence;
                c.dirty = true;
                ++stats_.inputs_run;
            }
            if (c.dirty && ni->owner != kNoPeer) {
                std::vector<u8> msg{kAck};
                PutU32(msg, it->first);
                PutU32(msg, c.last);
                for (f32 v : {t->position.x, t->position.y, t->position.z, m->velocity.x, m->velocity.y, m->velocity.z})
                    PutF32(msg, v);
                msg.push_back(m->grounded ? 1 : 0);
                if (host_.Send(ni->owner, config_.ack_channel, msg)) ++stats_.acks_sent;
                c.dirty = false;
            }
        }
        ++it;
    }
}

// ---------------------------------------------------------------- client

MovementClient::MovementClient(World& world, NetHost& host, ReplicationClient& replication, PeerId server,
                               PredictionConfig config)
    : world_(world), host_(host), replication_(replication), server_(server), config_(config), step_(DefaultStep()) {
    replication_.PredictLocally(GetComponentId<Transform>());
    replication_.PredictLocally(GetComponentId<NetMovement>());
}

usize MovementClient::Pending(u32 net_id) const {
    auto it = characters_.find(net_id);
    return it == characters_.end() ? 0 : it->second.pending.size();
}

bool MovementClient::Predict(Entity entity, f32 move_x, f32 move_z, bool jump, f32 dt) {
    if (!world_.IsAlive(entity)) return false;
    const NetIdentity* ni = world_.GetComponent<NetIdentity>(entity);
    Transform* t = world_.GetComponent<Transform>(entity);
    NetMovement* m = world_.GetComponent<NetMovement>(entity);
    if (!ni || !ni->locally_owned || !t || !m) return false;
    Character& c = characters_[ni->net_id];
    MovementInput input;
    input.sequence = c.next++;
    input.dt = dt, input.move_x = move_x, input.move_z = move_z, input.jump = jump;
    input = SanitizeInput(input, config_.max_dt);
    step_(world_, entity, *t, *m, input);
    c.pending.push_back(input);
    while (c.pending.size() > config_.max_pending) c.pending.pop_front();
    ++stats_.predicted;

    std::vector<u8> msg{kInputs};
    PutU32(msg, ni->net_id);
    const usize n = std::min(config_.redundancy, c.pending.size());
    msg.push_back(static_cast<u8>(n));
    for (usize i = c.pending.size() - n; i < c.pending.size(); ++i) {
        const MovementInput& p = c.pending[i];
        PutU32(msg, p.sequence);
        PutF32(msg, p.dt), PutF32(msg, p.move_x), PutF32(msg, p.move_z);
        msg.push_back(p.jump ? 1 : 0);
    }
    host_.Send(server_, config_.input_channel, msg);
    return true;
}

bool MovementClient::HandleEvent(const NetEvent& event) {
    if (event.type != NetEventType::Message || event.peer != server_ || event.channel != config_.ack_channel ||
        event.data.empty() || event.data[0] != kAck)
        return false;
    In in{event.data};
    const u32 net_id = in.U32(), sequence = in.U32();
    Vec3 position, velocity;
    position.x = in.F32(), position.y = in.F32(), position.z = in.F32();
    velocity.x = in.F32(), velocity.y = in.F32(), velocity.z = in.F32();
    const bool grounded = in.U8() != 0;
    if (!in.ok) return true;
    const Entity entity = replication_.FindEntity(net_id);
    if (entity.IsNull() || !world_.IsAlive(entity)) return true;
    Transform* t = world_.GetComponent<Transform>(entity);
    NetMovement* m = world_.GetComponent<NetMovement>(entity);
    const NetIdentity* ni = world_.GetComponent<NetIdentity>(entity);
    if (!t || !m || !ni || !ni->locally_owned) return true;
    Character& c = characters_[net_id];
    if (sequence <= c.acked) return true; // older than one already applied
    c.acked = sequence;
    ++stats_.acks;
    while (!c.pending.empty() && c.pending.front().sequence <= sequence) c.pending.pop_front();
    // Rewind to the server's state and replay what it hasn't reached yet.
    const Vec3 predicted = t->position;
    t->position = position;
    m->velocity = velocity;
    m->grounded = grounded;
    for (const MovementInput& input : c.pending) step_(world_, entity, *t, *m, input);
    const f32 error = (t->position - predicted).Length();
    stats_.last_error = error;
    stats_.max_error = std::max(stats_.max_error, error);
    if (error > config_.snap_distance) ++stats_.corrections;
    return true;
}

} // namespace aether::net
