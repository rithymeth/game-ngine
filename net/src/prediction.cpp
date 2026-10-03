#include "aether/net/prediction.h"

#include "aether/net/bytes.h"

#include <algorithm>
#include <cmath>

namespace aether::net {

namespace {

i8 Quantize(f32 v) { return static_cast<i8>(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f)); }
f32 Dequantize(i8 q) { return static_cast<f32>(q) / 127.0f; }

void WriteState(ByteWriter& w, const MoveState& s) {
    w.F32(s.position.x), w.F32(s.position.y), w.F32(s.position.z);
    w.F32(s.velocity.x), w.F32(s.velocity.y), w.F32(s.velocity.z);
    w.Bool(s.grounded);
}

MoveState ReadState(ByteReader& r) {
    MoveState s;
    s.position.x = r.F32(), s.position.y = r.F32(), s.position.z = r.F32();
    s.velocity.x = r.F32(), s.velocity.y = r.F32(), s.velocity.z = r.F32();
    s.grounded = r.Bool();
    return s;
}

bool Finite(const MoveState& s) {
    return std::isfinite(s.position.x) && std::isfinite(s.position.y) && std::isfinite(s.position.z) && std::isfinite(s.velocity.x) &&
           std::isfinite(s.velocity.y) && std::isfinite(s.velocity.z);
}

f32 Distance(const Vec3& a, const Vec3& b) { return (a - b).Length(); }

} // namespace

void StepMovement(const MoveSettings& s, const GroundFn& ground, MoveState& state, const MoveInput& input) {
    const f32 dt = s.dt;
    const f32 speed = input.run ? s.run_speed : s.walk_speed;
    const f32 target_x = input.move_x * speed;
    const f32 target_z = input.move_z * speed;
    const bool has_input = input.move_x != 0.0f || input.move_z != 0.0f;

    // Horizontal: accelerate toward the target (on the ground fully, in the air by air_control).
    f32 accel = state.grounded ? (has_input ? s.acceleration : s.braking) : s.air_acceleration;
    f32 want_x = target_x, want_z = target_z;
    if (!state.grounded) {
        want_x = state.velocity.x + (target_x - state.velocity.x) * s.air_control;
        want_z = state.velocity.z + (target_z - state.velocity.z) * s.air_control;
    }
    const f32 dx = want_x - state.velocity.x, dz = want_z - state.velocity.z;
    const f32 dist = std::sqrt(dx * dx + dz * dz);
    const f32 max_change = accel * dt;
    if (dist <= max_change || dist == 0.0f) {
        state.velocity.x = want_x;
        state.velocity.z = want_z;
    } else {
        state.velocity.x += dx / dist * max_change;
        state.velocity.z += dz / dist * max_change;
    }

    // Vertical.
    if (state.grounded && input.jump) {
        state.velocity.y = s.jump_velocity;
        state.grounded = false;
    }
    if (!state.grounded) state.velocity.y += s.gravity * dt;

    state.position.x += state.velocity.x * dt;
    state.position.z += state.velocity.z * dt;
    state.position.y += state.velocity.y * dt;

    const f32 floor = ground ? ground(state.position.x, state.position.z) : 0.0f;
    if (state.position.y <= floor) {
        state.position.y = floor;
        if (state.velocity.y < 0.0f) state.velocity.y = 0.0f;
        state.grounded = true;
    } else if (state.grounded && state.position.y > floor + 0.05f) {
        state.grounded = false; // walked off an edge
    }
}

MoveStepFn MakeMoveStep(const MoveSettings& settings, GroundFn ground) {
    return [settings, ground = std::move(ground)](MoveState& state, const MoveInput& input) {
        StepMovement(settings, ground, state, input);
    };
}

MoveInput Sanitize(const MoveInput& in) {
    MoveInput out = in;
    f32 x = std::isfinite(in.move_x) ? in.move_x : 0.0f;
    f32 z = std::isfinite(in.move_z) ? in.move_z : 0.0f;
    const f32 len = std::sqrt(x * x + z * z);
    if (len > 1.0f) {
        x /= len;
        z /= len;
    }
    out.move_x = Dequantize(Quantize(x));
    out.move_z = Dequantize(Quantize(z));
    return out;
}

// --- client -----------------------------------------------------------------

PredictionClient::PredictionClient(MoveStepFn step, const PredictionConfig& config) : step_(std::move(step)), config_(config) {}

void PredictionClient::Reset(const MoveState& state) {
    state_ = state;
    offset_ = Vec3(0, 0, 0);
    history_.clear();
    acknowledged_ = next_sequence_ - 1;
}

std::vector<u8> PredictionClient::Predict(const MoveInput& raw) {
    Entry e;
    e.sequence = next_sequence_++;
    e.input = Sanitize(raw);
    step_(state_, e.input);
    e.after = state_;
    history_.push_back(e);
    if (history_.size() > config_.max_history) history_.pop_front();
    ++stats_.inputs;

    const usize count = std::min(history_.size(), config_.redundancy + 1);
    ByteWriter w;
    w.U8(kInputMessage);
    w.Varint(count);
    w.Varint(history_[history_.size() - count].sequence);
    for (usize i = history_.size() - count; i < history_.size(); ++i) {
        const MoveInput& in = history_[i].input;
        w.U8(static_cast<u8>((in.jump ? 1 : 0) | (in.run ? 2 : 0)));
        w.U8(static_cast<u8>(Quantize(in.move_x)));
        w.U8(static_cast<u8>(Quantize(in.move_z)));
    }
    return w.Take();
}

bool PredictionClient::OnStateMessage(std::span<const u8> message) {
    ByteReader r(message);
    if (r.U8() != kStateMessage) return false;
    const u32 sequence = r.U32();
    const MoveState state = ReadState(r);
    if (!r.Ok() || r.Remaining() != 0 || !Finite(state)) return false;
    Reconcile(sequence, state);
    return true;
}

void PredictionClient::Reconcile(u32 ack, const MoveState& server) {
    if (ack < acknowledged_) return; // an old, reordered report
    acknowledged_ = ack;

    MoveState predicted;
    bool found = false;
    while (!history_.empty() && history_.front().sequence <= ack) {
        if (history_.front().sequence == ack) {
            predicted = history_.front().after;
            found = true;
        }
        history_.pop_front();
    }
    if (!found && ack == 0) return; // the server hasn't processed anything yet
    if (found) {
        const f32 error = Distance(predicted.position, server.position);
        if (error <= config_.position_tolerance && predicted.grounded == server.grounded) return;
        stats_.last_error = error;
    }

    const Vec3 before = state_.position;
    state_ = server;
    for (Entry& e : history_) {
        step_(state_, e.input);
        e.after = state_;
        ++stats_.replayed_inputs;
    }
    ++stats_.reconciliations;
    const f32 jump = Distance(before, state_.position);
    if (!found) stats_.last_error = jump;
    if (jump > config_.snap_distance) {
        offset_ = Vec3(0, 0, 0);
        ++stats_.snaps;
    } else {
        offset_ = offset_ + (before - state_.position); // the view stays where it was, then eases
    }
}

void PredictionClient::Update(f32 dt) {
    offset_ = offset_ * std::exp(-config_.correction_rate * dt);
    if (offset_.LengthSq() < 1e-8f) offset_ = Vec3(0, 0, 0);
}

// --- server -----------------------------------------------------------------

PredictionServer::PredictionServer(MoveStepFn step, const PredictionServerConfig& config) : step_(std::move(step)), config_(config) {}

void PredictionServer::AddPeer(NetAddress peer, const MoveState& initial) {
    Peer p;
    p.state = initial;
    peers_[peer] = std::move(p);
}

void PredictionServer::RemovePeer(NetAddress peer) { peers_.erase(peer); }

bool PredictionServer::OnInputMessage(NetAddress peer, std::span<const u8> message) {
    auto it = peers_.find(peer);
    if (it == peers_.end()) return false;
    Peer& p = it->second;

    ByteReader r(message);
    if (r.U8() != kInputMessage) {
        ++stats_.malformed;
        return false;
    }
    const u64 count = r.Varint();
    const u64 first = r.Varint();
    if (!r.Ok() || count == 0 || count > 16 || first == 0 || first > 0xFFFFFFFFull || r.Remaining() != count * 3) {
        ++stats_.malformed;
        return false;
    }
    for (u64 i = 0; i < count; ++i) {
        const u8 flags = r.U8();
        MoveInput in;
        in.jump = (flags & 1) != 0;
        in.run = (flags & 2) != 0;
        in.move_x = Dequantize(static_cast<i8>(r.U8()));
        in.move_z = Dequantize(static_cast<i8>(r.U8()));
        const u32 sequence = static_cast<u32>(first + i);

        // Redundant copies of inputs already queued or run are dropped, not errors.
        const bool seen = sequence <= p.last_processed ||
                          std::any_of(p.queue.begin(), p.queue.end(), [&](const auto& q) { return q.first == sequence; });
        if (seen || p.queue.size() >= config_.max_queued_inputs) {
            ++stats_.dropped;
            continue;
        }
        // A real client's inputs are already clamped; a bigger vector is a cheat or a bug.
        if (in.move_x * in.move_x + in.move_z * in.move_z > 1.02f * 1.02f) {
            in = Sanitize(in);
            ++stats_.clamped;
        }
        auto at = std::upper_bound(p.queue.begin(), p.queue.end(), sequence, [](u32 s, const auto& q) { return s < q.first; });
        p.queue.insert(at, {sequence, in});
    }
    return true;
}

void PredictionServer::Tick() {
    for (auto& [address, p] : peers_) {
        (void)address;
        p.credit = std::min(p.credit + 1.0f, static_cast<f32>(config_.max_burst));
        while (!p.queue.empty() && p.credit >= 1.0f) {
            step_(p.state, p.queue.front().second);
            p.last_processed = p.queue.front().first;
            p.queue.pop_front();
            p.credit -= 1.0f;
            ++stats_.processed;
        }
    }
}

const MoveState* PredictionServer::StateOf(NetAddress peer) const {
    auto it = peers_.find(peer);
    return it == peers_.end() ? nullptr : &it->second.state;
}

u32 PredictionServer::LastProcessed(NetAddress peer) const {
    auto it = peers_.find(peer);
    return it == peers_.end() ? 0 : it->second.last_processed;
}

usize PredictionServer::Queued(NetAddress peer) const {
    auto it = peers_.find(peer);
    return it == peers_.end() ? 0 : it->second.queue.size();
}

std::vector<u8> PredictionServer::BuildStateMessage(NetAddress peer) const {
    auto it = peers_.find(peer);
    if (it == peers_.end()) return {};
    ByteWriter w;
    w.U8(kStateMessage);
    w.U32(it->second.last_processed);
    WriteState(w, it->second.state);
    return w.Take();
}

void PredictionServer::SetState(NetAddress peer, const MoveState& state) {
    auto it = peers_.find(peer);
    if (it != peers_.end()) it->second.state = state;
}

} // namespace aether::net
