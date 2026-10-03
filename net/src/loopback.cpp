#include "aether/net/transport.h"

#include <algorithm>

namespace aether::net {

bool LoopbackTransport::Send(NetAddress to, std::span<const u8> data) {
    if (data.size() > MaxDatagramSize() || to == 0 || to > network_.endpoints_.size()) return false;
    network_.Enqueue(address_, to, data);
    return true;
}

bool LoopbackTransport::Broadcast(u16 port, std::span<const u8> data) {
    (void)port;
    if (data.size() > MaxDatagramSize()) return false;
    for (NetAddress to = 1; to <= network_.endpoints_.size(); ++to) {
        if (to != address_) network_.Enqueue(address_, to, data);
    }
    return true;
}

bool LoopbackTransport::Receive(Datagram& out) {
    if (inbox_.empty()) return false;
    out = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
}

LoopbackTransport& LoopbackNetwork::CreateEndpoint() {
    const NetAddress address = static_cast<NetAddress>(endpoints_.size() + 1);
    endpoints_.push_back(std::unique_ptr<LoopbackTransport>(new LoopbackTransport(*this, address)));
    return *endpoints_.back();
}

f64 LoopbackNetwork::Random() {
    rng_ ^= rng_ << 13; // xorshift64
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    return static_cast<f64>(rng_ >> 11) / 9007199254740992.0;
}

void LoopbackNetwork::Enqueue(NetAddress from, NetAddress to, std::span<const u8> data) {
    if (Random() < conditions.loss) {
        ++dropped_;
        return;
    }
    const int copies = Random() < conditions.duplicate ? 2 : 1;
    for (int i = 0; i < copies; ++i) {
        Pending p;
        p.deliver_at = now_ + conditions.latency + Random() * conditions.jitter;
        p.order = order_++;
        p.to = to;
        p.datagram.from = from;
        p.datagram.data.assign(data.begin(), data.end());
        in_flight_.push_back(std::move(p));
    }
    if (conditions.latency <= 0.0 && conditions.jitter <= 0.0) Deliver(); // no delay: usable without Advance
}

void LoopbackNetwork::Deliver() {
    std::stable_sort(in_flight_.begin(), in_flight_.end(), [](const Pending& a, const Pending& b) {
        return a.deliver_at != b.deliver_at ? a.deliver_at < b.deliver_at : a.order < b.order;
    });
    usize due = 0;
    while (due < in_flight_.size() && in_flight_[due].deliver_at <= now_) {
        Pending& p = in_flight_[due];
        endpoints_[p.to - 1]->inbox_.push_back(std::move(p.datagram));
        ++due;
    }
    in_flight_.erase(in_flight_.begin(), in_flight_.begin() + static_cast<std::ptrdiff_t>(due));
}

void LoopbackNetwork::Advance(f64 dt) {
    now_ += dt;
    Deliver();
}

} // namespace aether::net
