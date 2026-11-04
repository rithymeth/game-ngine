#include "aether/physics_net/client.h"

namespace aether::physics_net {

u16 PhysicsNetClient::RequestGrab(const EntityGuid& object, const Vec3& point) {
    GrabRequest request;
    request.request_id = next_request_++;
    if (next_request_ == 0) next_request_ = 1;
    request.object = object;
    request.point = point;
    Entry& entry = grabs_[object];
    entry.state = ClientGrabState::Pending;
    entry.request_id = request.request_id;
    host_.Send(server_, kReliableChannel, Encode(request));
    return request.request_id;
}

bool PhysicsNetClient::SetTarget(const EntityGuid& object, const Vec3& target) {
    const auto it = grabs_.find(object);
    if (it == grabs_.end() || it->second.state != ClientGrabState::Held) return false;
    GrabTarget message;
    message.sequence = ++target_sequence_;
    message.object = object;
    message.target = target;
    return host_.Send(server_, kSequencedChannel, Encode(message));
}

bool PhysicsNetClient::Release(const EntityGuid& object) {
    const auto it = grabs_.find(object);
    if (it == grabs_.end() || (it->second.state != ClientGrabState::Held && it->second.state != ClientGrabState::Pending)) return false;
    GrabRelease message;
    message.object = object;
    host_.Send(server_, kReliableChannel, Encode(message));
    it->second.state = ClientGrabState::None;
    return true;
}

ClientGrabState PhysicsNetClient::StateOf(const EntityGuid& object) const {
    const auto it = grabs_.find(object);
    return it == grabs_.end() ? ClientGrabState::None : it->second.state;
}

GrabStatus PhysicsNetClient::LastStatus(const EntityGuid& object) const {
    const auto it = grabs_.find(object);
    return it == grabs_.end() ? GrabStatus::Granted : it->second.status;
}

bool PhysicsNetClient::HandleEvent(const net::NetEvent& event) {
    if (event.type != net::NetEventType::Message || event.peer != server_) return false;
    const std::optional<MessageType> type = PeekMessageType(event.data);
    if (!type) return false;
    if (*type == MessageType::SnapshotChunk) {
        SnapshotChunk chunk;
        if (!Decode(event.data, chunk)) return ++stats_.malformed, true;
        ++stats_.chunks_received;
        // Snapshot numbers wrap, so "older" is the sign of the difference.
        if (have_snapshot_ && static_cast<i32>(chunk.sequence - newest_snapshot_) < 0) return ++stats_.chunks_stale, true;
        newest_snapshot_ = chunk.sequence;
        have_snapshot_ = true;
        scene_.ApplyConstraintState(chunk.snapshot);
        ++stats_.chunks_applied;
        return true;
    }
    if (*type == MessageType::GrabResult) {
        GrabResult result;
        if (!Decode(event.data, result)) return ++stats_.malformed, true;
        Entry& entry = grabs_[result.object];
        if (result.status == GrabStatus::Granted) {
            // Only the answer to our latest request counts (an old one can arrive after we asked again or let go).
            if (entry.request_id != result.request_id || entry.state != ClientGrabState::Pending) return true;
            entry.state = ClientGrabState::Held;
            entry.status = GrabStatus::Granted;
            ++stats_.grabs_granted;
        } else if (result.status == GrabStatus::Released) {
            entry.state = ClientGrabState::None;
            entry.status = GrabStatus::Released;
            ++stats_.grabs_released_by_host;
        } else {
            if (entry.request_id != result.request_id) return true;
            entry.state = ClientGrabState::Denied;
            entry.status = result.status;
            ++stats_.grabs_denied;
        }
        return true;
    }
    return false;
}

} // namespace aether::physics_net
