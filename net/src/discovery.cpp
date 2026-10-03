#include "aether/net/discovery.h"

#include "aether/net/bytes.h"

#include <algorithm>

namespace aether::net {

namespace {

constexpr u32 kDiscoveryMagic = 0x53444541; // "AEDS"

bool ShortEnough(const std::string& s) { return s.size() <= kMaxDiscoveryString; }

} // namespace

void LanHost::Update(f64 now) {
    if (now - window_start_ >= 1.0) {
        window_start_ = now;
        window_count_ = 0;
    }
    Datagram d;
    while (transport_.Receive(d)) {
        if (d.data.empty() || d.data[0] != kDiscoveryQuery) continue; // not ours
        ByteReader r({d.data.data() + 1, d.data.size() - 1});
        const u32 magic = r.U32();
        const std::string game = r.String();
        const u32 nonce = r.U32();
        if (!r.Ok() || r.Remaining() != 0 || magic != kDiscoveryMagic || !ShortEnough(game)) {
            ++ignored_;
            continue;
        }
        if (!advertising_ || game != game_) continue; // a different game's query: stay quiet
        if (max_replies_per_second != 0 && window_count_ >= max_replies_per_second) {
            ++ignored_;
            continue;
        }
        ++window_count_;

        ByteWriter w;
        w.U8(kDiscoveryResponse);
        w.U32(kDiscoveryMagic);
        w.U32(nonce);
        w.String(game_);
        w.String(info_.name.substr(0, kMaxDiscoveryString));
        w.String(info_.map.substr(0, kMaxDiscoveryString));
        w.U32(info_.game_version);
        w.U16(info_.game_port);
        w.U8(info_.players);
        w.U8(info_.max_players);
        if (transport_.Send(d.from, w.Data())) ++answered_;
    }
}

std::vector<u8> LanBrowser::NextQuery(f64 now) {
    ++nonce_;
    sent_at_ = now;
    ByteWriter w;
    w.U8(kDiscoveryQuery);
    w.U32(kDiscoveryMagic);
    w.String(game_);
    w.U32(nonce_);
    return w.Take();
}

bool LanBrowser::Search(f64 now, u16 port) { return transport_.Broadcast(port, NextQuery(now)); }

bool LanBrowser::SearchHost(NetAddress host, f64 now) { return transport_.Send(host, NextQuery(now)); }

void LanBrowser::Update(f64 now) {
    Datagram d;
    while (transport_.Receive(d)) {
        if (d.data.empty() || d.data[0] != kDiscoveryResponse) continue;
        ByteReader r({d.data.data() + 1, d.data.size() - 1});
        const u32 magic = r.U32();
        const u32 nonce = r.U32();
        const std::string game = r.String();
        DiscoveredSession s;
        s.host = d.from;
        s.info.name = r.String();
        s.info.map = r.String();
        s.info.game_version = r.U32();
        s.info.game_port = r.U16();
        s.info.players = r.U8();
        s.info.max_players = r.U8();
        if (!r.Ok() || r.Remaining() != 0 || magic != kDiscoveryMagic || !ShortEnough(game) || !ShortEnough(s.info.name) ||
            !ShortEnough(s.info.map) || s.info.game_port == 0 || s.info.players > s.info.max_players) {
            ++malformed_;
            continue;
        }
        if (game != game_) continue;
        s.last_seen = now;
        s.round_trip = nonce == nonce_ ? std::max(0.0, now - sent_at_) : 0.0;
        auto existing = std::find_if(sessions_.begin(), sessions_.end(), [&](const DiscoveredSession& e) { return e.host == s.host; });
        if (existing != sessions_.end()) {
            if (s.round_trip == 0.0) s.round_trip = existing->round_trip;
            *existing = s;
        } else {
            sessions_.push_back(s);
        }
    }
}

void LanBrowser::Prune(f64 now, f64 max_age) {
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [&](const DiscoveredSession& s) { return now - s.last_seen > max_age; }),
                    sessions_.end());
}

} // namespace aether::net
