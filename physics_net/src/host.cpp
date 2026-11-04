#include "aether/physics_net/host.h"

#include <algorithm>
#include <vector>

namespace aether::physics_net {

PhysicsNetHost::PhysicsNetHost(PhysicsScene& scene, net::NetHost& host, const GuidIndex& guids, PhysicsNetHostConfig config)
    : scene_(scene), host_(host), guids_(guids), config_(std::move(config)) {}

Vec3 PhysicsNetHost::PositionOf(Entity entity) const {
    const Transform* t = scene_.EcsWorld().GetComponent<Transform>(entity);
    return t ? t->position : Vec3(0, 0, 0);
}

void PhysicsNetHost::SetPlayer(net::PeerId peer, Entity pawn) {
    Player& player = players_[peer];
    if (player.pawn != pawn) ReleaseAll(player);
    player.pawn = pawn;
}

void PhysicsNetHost::ReleaseAll(Player& player) {
    for (auto& [guid, grab] : player.grabs) scene_.Release(grab.id);
    player.grabs.clear();
}

void PhysicsNetHost::RemovePlayer(net::PeerId peer) {
    const auto it = players_.find(peer);
    if (it == players_.end()) return;
    ReleaseAll(it->second);
    players_.erase(it);
}

usize PhysicsNetHost::GrabsOf(net::PeerId peer) const {
    const auto it = players_.find(peer);
    return it == players_.end() ? 0 : it->second.grabs.size();
}

void PhysicsNetHost::Reply(net::PeerId peer, u16 request_id, GrabStatus status, const EntityGuid& object) {
    GrabResult result;
    result.request_id = request_id;
    result.status = status;
    result.object = object;
    host_.Send(peer, kReliableChannel, Encode(result));
}

bool PhysicsNetHost::HandleEvent(const net::NetEvent& event) {
    if (event.type == net::NetEventType::Disconnected) {
        RemovePlayer(event.peer);
        return false;
    }
    if (event.type != net::NetEventType::Message) return false;
    const std::optional<MessageType> type = PeekMessageType(event.data);
    if (!type) return false;
    switch (*type) {
    case MessageType::GrabRequest: {
        GrabRequest m;
        if (!Decode(event.data, m)) return ++stats_.malformed, true;
        OnRequest(event.peer, m);
        return true;
    }
    case MessageType::GrabTarget: {
        GrabTarget m;
        if (!Decode(event.data, m)) return ++stats_.malformed, true;
        OnTarget(event.peer, m);
        return true;
    }
    case MessageType::GrabRelease: {
        GrabRelease m;
        if (!Decode(event.data, m)) return ++stats_.malformed, true;
        OnRelease(event.peer, m);
        return true;
    }
    default:
        return false; // snapshots and results go the other way: not ours to handle on a host
    }
}

void PhysicsNetHost::OnRequest(net::PeerId peer, const GrabRequest& request) {
    ++stats_.requests;
    const GrabPolicy& policy = config_.policy;
    GrabFacts facts;
    const auto player = players_.find(peer);
    facts.player_known = player != players_.end() && scene_.EcsWorld().IsAlive(player->second.pawn);
    Entity object;
    if (facts.player_known) {
        facts.rate_ok = player->second.bucket.Take(now_, policy.requests_per_second, policy.request_burst);
        facts.peer_grabs = static_cast<u32>(player->second.grabs.size());
        facts.already_holding_object = player->second.grabs.count(request.object) != 0;
        facts.holder_position = PositionOf(player->second.pawn);
    }
    object = guids_.Find(scene_.EcsWorld(), request.object);
    const World& world = scene_.EcsWorld();
    if (!object.IsNull() && world.IsAlive(object)) {
        const RigidBody* body = world.GetComponent<RigidBody>(object);
        facts.object_known = body != nullptr && body->motion == BodyMotion::Dynamic && !scene_.BodyOf(object).IsInvalid();
        facts.grabbable = !filter_ || filter_(object);
        facts.object_position = PositionOf(object);
        facts.object_holders = static_cast<u32>(scene_.GrabCount(object));
    }
    facts.point = request.point;
    GrabStatus status = ValidateGrab(policy, facts);
    if (status == GrabStatus::Granted) {
        const GrabId id = scene_.Grab(object, player->second.pawn, request.point, policy.settings);
        if (id == kInvalidGrab) {
            status = GrabStatus::UnknownObject;
        } else {
            HeldGrab held;
            held.id = id;
            held.object = object;
            held.deadline = now_ + policy.first_target_grace;
            player->second.grabs[request.object] = held;
        }
    }
    if (status == GrabStatus::Granted) ++stats_.granted;
    else ++stats_.denied, ++stats_.denied_by[static_cast<usize>(status)];
    Reply(peer, request.request_id, status, request.object);
}

void PhysicsNetHost::OnTarget(net::PeerId peer, const GrabTarget& target) {
    const auto player = players_.find(peer);
    if (player == players_.end()) return void(++stats_.targets_ignored);
    const auto grab = player->second.grabs.find(target.object);
    if (grab == player->second.grabs.end()) return void(++stats_.targets_ignored);
    HeldGrab& held = grab->second;
    // Newest wins: an older (or repeated) update that arrives late is dropped. Sequence numbers wrap, so compare the difference.
    if (held.has_sequence && static_cast<i32>(target.sequence - held.last_sequence) <= 0) return void(++stats_.targets_stale);
    held.last_sequence = target.sequence;
    held.has_sequence = true;
    held.deadline = now_ + config_.policy.target_timeout;
    const Vec3 holder = PositionOf(player->second.pawn);
    const Vec3 clamped = ClampGrabTarget(config_.policy, holder, target.target);
    if (clamped.x != target.target.x || clamped.y != target.target.y || clamped.z != target.target.z) ++stats_.targets_clamped;
    scene_.SetGrabTarget(held.id, clamped);
    ++stats_.targets_applied;
}

void PhysicsNetHost::OnRelease(net::PeerId peer, const GrabRelease& release) {
    const auto player = players_.find(peer);
    if (player == players_.end()) return;
    const auto grab = player->second.grabs.find(release.object);
    if (grab == player->second.grabs.end()) return;
    scene_.Release(grab->second.id);
    player->second.grabs.erase(grab);
    ++stats_.releases;
}

void PhysicsNetHost::Tick(f64 now) {
    now_ = now;
    // Grab upkeep: a grab the physics ended (it broke, or its object went away) or whose client has gone quiet is dropped, and the client told.
    for (auto& [peer, player] : players_) {
        std::vector<EntityGuid> ended;
        std::vector<bool> timed_out;
        for (const auto& [guid, grab] : player.grabs) {
            const bool alive = scene_.IsGrabActive(grab.id);
            const bool quiet = now > grab.deadline;
            if (!alive || quiet) {
                ended.push_back(guid);
                timed_out.push_back(alive && quiet);
            }
        }
        for (usize i = 0; i < ended.size(); ++i) {
            const auto it = player.grabs.find(ended[i]);
            if (timed_out[i]) scene_.Release(it->second.id), ++stats_.timeouts;
            else ++stats_.lost_grabs;
            player.grabs.erase(it);
            Reply(peer, 0, GrabStatus::Released, ended[i]);
        }
    }
    if (now >= next_snapshot_) {
        next_snapshot_ = now + config_.snapshot_interval;
        const std::vector<std::vector<u8>> chunks =
            EncodeSnapshotChunks(snapshot_sequence_++, scene_.CaptureConstraintState(), config_.snapshot_chunk_bytes);
        ++stats_.snapshots_sent;
        for (const std::vector<u8>& chunk : chunks) {
            host_.Broadcast(kUnreliableChannel, chunk);
            ++stats_.chunks_sent;
        }
    }
}

} // namespace aether::physics_net
