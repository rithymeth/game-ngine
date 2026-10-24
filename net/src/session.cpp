#include "aether/net/session.h"

#include <algorithm>
#include <chrono>

#include "aether/net/siphash.h"

namespace aether::net {

namespace {

constexpr u32 kQueryMagic = 0x51444541;  // "AEDQ"
constexpr u32 kAnswerMagic = 0x52444541; // "AEDR"
constexpr u8 kJoinRequest = 6, kJoinAccept = 7, kJoinRefuse = 8;

struct Out {
    std::vector<u8> bytes;
    void U8(u8 v) { bytes.push_back(v); }
    void U16(u16 v) { U8(static_cast<u8>(v)), U8(static_cast<u8>(v >> 8)); }
    void U32(u32 v) { U16(static_cast<u16>(v)), U16(static_cast<u16>(v >> 16)); }
    void U64(u64 v) { U32(static_cast<u32>(v)), U32(static_cast<u32>(v >> 32)); }
    void Str(const std::string& s) {
        const usize n = std::min<usize>(s.size(), 255);
        U8(static_cast<u8>(n));
        bytes.insert(bytes.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
    }
};

struct In {
    std::span<const u8> data;
    usize at = 0;
    bool ok = true;
    bool Has(usize n) { return ok = ok && at + n <= data.size(); }
    u8 U8() { return Has(1) ? data[at++] : 0; }
    u16 U16() { u16 lo = U8(); return static_cast<u16>(lo | (U8() << 8)); }
    u32 U32() { u32 lo = U16(); return lo | (static_cast<u32>(U16()) << 16); }
    u64 U64() { u64 lo = U32(); return lo | (static_cast<u64>(U32()) << 32); }
    std::string Str() {
        const u8 n = U8();
        if (!Has(n)) return {};
        std::string s(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        return s;
    }
};

void WriteInfo(Out& o, const SessionInfo& info) {
    o.Str(info.name), o.Str(info.map), o.Str(info.mode);
    o.U16(info.port), o.U32(info.players), o.U32(info.max_players), o.U32(info.build), o.U8(info.password ? 1 : 0);
    const usize n = std::min<usize>(info.properties.size(), 255);
    o.U8(static_cast<u8>(n));
    for (usize i = 0; i < n; ++i) o.Str(info.properties[i].first), o.Str(info.properties[i].second);
}

bool ReadInfo(In& in, SessionInfo& info) {
    info = {};
    info.name = in.Str(), info.map = in.Str(), info.mode = in.Str();
    info.port = in.U16(), info.players = in.U32(), info.max_players = in.U32(), info.build = in.U32();
    info.password = in.U8() != 0;
    const u8 n = in.U8();
    for (u8 i = 0; i < n && in.ok; ++i) {
        std::string k = in.Str();
        info.properties.emplace_back(std::move(k), in.Str());
    }
    return in.ok;
}

} // namespace

const std::string* SessionInfo::Property(const std::string& key) const {
    for (const auto& [k, v] : properties)
        if (k == key) return &v;
    return nullptr;
}

std::vector<u8> EncodeSessionInfo(const SessionInfo& info) {
    Out o;
    WriteInfo(o, info);
    return o.bytes;
}

bool DecodeSessionInfo(std::span<const u8> data, SessionInfo& out) {
    In in{data};
    return ReadInfo(in, out) && in.at == data.size();
}

const char* JoinResultName(JoinResult result) {
    switch (result) {
    case JoinResult::Ok: return "Ok";
    case JoinResult::WrongPassword: return "WrongPassword";
    case JoinResult::BuildMismatch: return "BuildMismatch";
    case JoinResult::Full: return "Full";
    case JoinResult::NoRequest: return "NoRequest";
    case JoinResult::Kicked: return "Kicked";
    case JoinResult::ConnectFailed: return "ConnectFailed";
    case JoinResult::Lost: return "Lost";
    case JoinResult::BadToken: return "BadToken";
    case JoinResult::TokenExpired: return "TokenExpired";
    }
    return "?";
}

// ---------------------------------------------------------------- join tokens

namespace {
SipKey TokenKey(const std::string& secret) {
    // Two SipHash calls under fixed, domain-separated keys stretch the secret into a 128-bit key.
    SipKey a{}, b{};
    const char* da = "aether.token.k0!";
    const char* db = "aether.token.k1!";
    std::copy_n(da, 16, a.begin()), std::copy_n(db, 16, b.begin());
    const std::span<const u8> s(reinterpret_cast<const u8*>(secret.data()), secret.size());
    const u64 lo = SipHash24(a, s), hi = SipHash24(b, s);
    SipKey k{};
    for (int i = 0; i < 8; ++i) k[i] = static_cast<u8>(lo >> (8 * i)), k[8 + i] = static_cast<u8>(hi >> (8 * i));
    return k;
}
std::string Hex(std::span<const u8> b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (u8 x : b) s += d[x >> 4], s += d[x & 15];
    return s;
}
bool Unhex(const std::string& s, std::vector<u8>& out) {
    if (s.size() % 2) return false;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    out.clear();
    for (usize i = 0; i < s.size(); i += 2) {
        const int h = v(s[i]), l = v(s[i + 1]);
        if (h < 0 || l < 0) return false;
        out.push_back(static_cast<u8>(h * 16 + l));
    }
    return true;
}
} // namespace

u64 UnixNow() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

const char* TokenCheckName(TokenCheck c) {
    switch (c) {
    case TokenCheck::Ok: return "Ok";
    case TokenCheck::Malformed: return "Malformed";
    case TokenCheck::BadSignature: return "BadSignature";
    case TokenCheck::Expired: return "Expired";
    case TokenCheck::WrongBuild: return "WrongBuild";
    }
    return "?";
}

std::string IssueJoinToken(const std::string& issuer_secret, const JoinToken& token) {
    Out o;
    o.U8(1), o.U64(token.expires), o.U32(token.build);
    o.Str(token.player.substr(0, 64));
    const u64 tag = SipHash24(TokenKey(issuer_secret), o.bytes);
    Out t;
    t.U64(tag);
    return "aet1." + Hex(o.bytes) + "." + Hex(t.bytes);
}

TokenCheck VerifyJoinToken(const std::string& issuer_secret, const std::string& text, u32 build, u64 now_unix, JoinToken* out) {
    if (text.size() > 255 || text.rfind("aet1.", 0) != 0) return TokenCheck::Malformed;
    const usize dot = text.find('.', 5);
    if (dot == std::string::npos) return TokenCheck::Malformed;
    std::vector<u8> payload, tag;
    if (!Unhex(text.substr(5, dot - 5), payload) || !Unhex(text.substr(dot + 1), tag) || tag.size() != 8) return TokenCheck::Malformed;
    In tin{tag};
    if (!TagEqual(SipHash24(TokenKey(issuer_secret), payload), tin.U64())) return TokenCheck::BadSignature;
    In in{payload};
    JoinToken t;
    const u8 version = in.U8();
    t.expires = in.U64(), t.build = in.U32(), t.player = in.Str();
    if (!in.ok || version != 1 || in.at != payload.size()) return TokenCheck::Malformed;
    if (now_unix >= t.expires) return TokenCheck::Expired;
    if (t.build != build) return TokenCheck::WrongBuild;
    if (out) *out = std::move(t);
    return TokenCheck::Ok;
}

// ---------------------------------------------------------------- LAN discovery

void LanBeacon::Update() {
    Address from;
    std::vector<u8> data;
    while (socket_.Receive(from, data)) {
        In in{data};
        const u32 magic = in.U32(), protocol = in.U32();
        const u64 nonce = in.U64();
        if (!in.ok || magic != kQueryMagic || protocol != protocol_ || !enabled) continue;
        Out o;
        o.U32(kAnswerMagic), o.U32(protocol_), o.U64(nonce);
        WriteInfo(o, info);
        socket_.Send(from, o.bytes);
        ++answered_;
    }
}

LanBrowser::LanBrowser(DatagramSocket& socket, u32 build, u16 discovery_port, u32 protocol_id)
    : socket_(socket), build_(build), port_(discovery_port), protocol_(protocol_id),
      nonce_seed_(static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0x9E3779B97F4A7C15ull) {}

void LanBrowser::Search(f64 now) {
    nonce_seed_ = nonce_seed_ * 6364136223846793005ull + 1442695040888963407ull;
    const u64 nonce = nonce_seed_;
    Out o;
    o.U32(kQueryMagic), o.U32(protocol_), o.U64(nonce);
    socket_.Send(Address::Broadcast(port_), o.bytes);
    queries_.push_back({nonce, now});
    if (queries_.size() > 16) queries_.erase(queries_.begin());
    last_search_ = now;
}

void LanBrowser::Update(f64 now) {
    if (auto_search > 0.0 && now - last_search_ >= auto_search) Search(now);
    Address from;
    std::vector<u8> data;
    while (socket_.Receive(from, data)) {
        In in{data};
        const u32 magic = in.U32(), protocol = in.U32();
        const u64 nonce = in.U64();
        SessionInfo info;
        if (!in.ok || magic != kAnswerMagic || protocol != protocol_ || !ReadInfo(in, info)) continue;
        auto q = std::find_if(queries_.begin(), queries_.end(), [&](const auto& x) { return x.first == nonce; });
        if (q == queries_.end()) continue; // not an answer to us
        const Address game{from.ip, info.port != 0 ? info.port : from.port};
        auto it = std::find_if(sessions_.begin(), sessions_.end(), [&](const LanSession& s) { return s.address == game; });
        if (it == sessions_.end()) it = sessions_.insert(sessions_.end(), LanSession{});
        it->address = game;
        it->compatible = info.build == build_;
        it->info = std::move(info);
        it->ping = now - q->second;
        it->last_seen = now;
    }
    std::erase_if(sessions_, [&](const LanSession& s) { return now - s.last_seen > expiry; });
}

std::vector<LanSession> LanBrowser::Results() const {
    std::vector<LanSession> out = sessions_;
    std::stable_sort(out.begin(), out.end(), [](const LanSession& a, const LanSession& b) {
        return a.ping != b.ping ? a.ping < b.ping : a.address < b.address;
    });
    return out;
}

// ---------------------------------------------------------------- hosting

SessionHost::SessionHost(NetHost& host, SessionInfo info, std::string password, SessionConfig config)
    : host_(host), info_(std::move(info)), password_(std::move(password)), config_(config) {
    info_.password = !password_.empty();
    info_.players = 0;
}

const SessionPlayer* SessionHost::Player(PeerId peer) const {
    for (const SessionPlayer& p : players_)
        if (p.peer == peer) return &p;
    return nullptr;
}

void SessionHost::Refuse(PeerId peer, JoinResult reason, f64 now) {
    const u8 msg[2] = {kJoinRefuse, static_cast<u8>(reason)};
    host_.Send(peer, config_.channel, msg);
    leaving_.push_back({peer, now});
}

void SessionHost::Kick(PeerId peer, f64 now) {
    auto it = std::find_if(players_.begin(), players_.end(), [&](const SessionPlayer& p) { return p.peer == peer; });
    if (it == players_.end()) return;
    events_.push_back({SessionEventType::PlayerLeft, peer, it->name, JoinResult::Kicked});
    players_.erase(it);
    info_.players = static_cast<u32>(players_.size());
    Refuse(peer, JoinResult::Kicked, now);
}

bool SessionHost::HandleEvent(const NetEvent& event, f64 now) {
    if (event.type == NetEventType::Connected) {
        pending_.push_back({event.peer, now});
        return false;
    }
    if (event.type == NetEventType::Disconnected) {
        std::erase_if(pending_, [&](const auto& p) { return p.first == event.peer; });
        std::erase_if(leaving_, [&](const auto& p) { return p.first == event.peer; });
        auto it = std::find_if(players_.begin(), players_.end(), [&](const SessionPlayer& p) { return p.peer == event.peer; });
        if (it != players_.end()) {
            events_.push_back({SessionEventType::PlayerLeft, event.peer, it->name, JoinResult::Lost});
            players_.erase(it);
            info_.players = static_cast<u32>(players_.size());
        }
        return false;
    }
    if (event.type != NetEventType::Message || event.channel != config_.channel || event.data.empty() ||
        event.data[0] != kJoinRequest)
        return false;
    auto pending = std::find_if(pending_.begin(), pending_.end(), [&](const auto& p) { return p.first == event.peer; });
    if (pending == pending_.end()) return true; // asked twice, or already refused
    pending_.erase(pending);
    In in{event.data};
    in.U8();
    const u32 build = in.U32();
    std::string name = in.Str();
    const std::string password = in.Str();
    // Newer clients append a token (possibly empty); older requests end at the password.
    const std::string token = in.ok && in.at < event.data.size() ? in.Str() : std::string();
    JoinResult refuse = JoinResult::Ok;
    JoinToken claims;
    TokenCheck check = TokenCheck::Ok;
    const bool check_token = !tokens_.issuer_secret.empty() && (tokens_.required || !token.empty());
    if (check_token) check = VerifyJoinToken(tokens_.issuer_secret, token, info_.build, tokens_.clock ? tokens_.clock() : UnixNow(), &claims);
    const u32 max = info_.max_players != 0 ? info_.max_players : host_.Config().max_peers;
    if (!in.ok || build != info_.build) refuse = JoinResult::BuildMismatch;
    else if (check_token && check == TokenCheck::Expired) refuse = JoinResult::TokenExpired;
    else if (check_token && check != TokenCheck::Ok) refuse = JoinResult::BadToken;
    else if (!password_.empty() && password != password_) refuse = JoinResult::WrongPassword;
    else if (players_.size() >= max) refuse = JoinResult::Full;
    if (refuse != JoinResult::Ok) {
        events_.push_back({SessionEventType::JoinRefused, event.peer, name, refuse});
        Refuse(event.peer, refuse, now);
        return true;
    }
    if (name.empty()) name = "Player";
    const std::string base = name;
    for (int n = 2; std::any_of(players_.begin(), players_.end(), [&](const SessionPlayer& p) { return p.name == name; }); ++n)
        name = base + " (" + std::to_string(n) + ")";
    players_.push_back({event.peer, name, check_token ? claims.player : std::string()});
    info_.players = static_cast<u32>(players_.size());
    Out o;
    o.U8(kJoinAccept);
    o.Str(name);
    WriteInfo(o, info_);
    host_.Send(event.peer, config_.channel, o.bytes);
    events_.push_back({SessionEventType::PlayerJoined, event.peer, name, JoinResult::Ok});
    return true;
}

void SessionHost::Update(f64 now) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now - it->second >= config_.join_timeout) {
            const PeerId peer = it->first;
            it = pending_.erase(it);
            events_.push_back({SessionEventType::JoinRefused, peer, {}, JoinResult::NoRequest});
            Refuse(peer, JoinResult::NoRequest, now);
        } else {
            ++it;
        }
    }
    for (auto it = leaving_.begin(); it != leaving_.end();) {
        const std::optional<PeerInfo> peer = host_.Peer(it->first);
        const bool through = peer && peer->connection && peer->connection->Unacked() == 0 && now > it->second;
        if (!peer || through || now - it->second >= config_.linger) {
            if (peer) host_.Disconnect(it->first, now);
            it = leaving_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<SessionEvent> SessionHost::TakeEvents() { return std::exchange(events_, {}); }

// ---------------------------------------------------------------- joining

void SessionClient::Join(const Address& server, const std::string& player_name, const std::string& password, u32 build,
                         f64 now, const std::string& token) {
    name_ = player_name, password_ = password, token_ = token, build_ = build;
    result_ = JoinResult::Ok;
    info_ = {};
    server_ = host_.Connect(server, now);
    state_ = State::Connecting;
}

void SessionClient::Leave(f64 now) {
    if (server_ != kNoPeer) host_.Disconnect(server_, now);
    server_ = kNoPeer;
    state_ = State::Idle;
}

bool SessionClient::HandleEvent(const NetEvent& event) {
    if (server_ == kNoPeer || event.peer != server_) return false;
    if (event.type == NetEventType::Connected && state_ == State::Connecting) {
        Out o;
        o.U8(kJoinRequest);
        o.U32(build_);
        o.Str(name_);
        o.Str(password_);
        o.Str(token_);
        host_.Send(server_, config_.channel, o.bytes);
        return false;
    }
    if (event.type == NetEventType::Disconnected) {
        if (state_ == State::Connecting)
            result_ = event.reason == DisconnectReason::Denied ? JoinResult::Full : JoinResult::ConnectFailed;
        else if (state_ == State::Joined)
            result_ = JoinResult::Lost;
        if (state_ != State::Failed && state_ != State::Idle) state_ = State::Failed;
        server_ = kNoPeer;
        return false;
    }
    if (event.type != NetEventType::Message || event.channel != config_.channel || event.data.empty()) return false;
    In in{event.data};
    const u8 kind = in.U8();
    if (kind == kJoinAccept && state_ == State::Connecting) {
        std::string name = in.Str();
        SessionInfo info;
        if (ReadInfo(in, info)) {
            name_ = std::move(name), info_ = std::move(info);
            state_ = State::Joined;
        }
        return true;
    }
    if (kind == kJoinRefuse) {
        const u8 reason = in.U8();
        result_ = in.ok && reason <= static_cast<u8>(JoinResult::TokenExpired) ? static_cast<JoinResult>(reason) : JoinResult::Lost;
        state_ = State::Failed;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- lobbies

LanLobbyService::LanLobbyService(DatagramSocket* discovery, DatagramSocket& search, u32 build, u16 discovery_port,
                                 u32 protocol_id)
    : browser_(search, build, discovery_port, protocol_id) {
    if (discovery) beacon_ = std::make_unique<LanBeacon>(*discovery, protocol_id);
}

bool LanLobbyService::Advertise(const SessionInfo& info) {
    if (!beacon_) return false;
    beacon_->info = info;
    advertising_ = true;
    return true;
}

void LanLobbyService::Update(f64 now) {
    if (beacon_) {
        beacon_->enabled = advertising_;
        beacon_->Update();
    }
    browser_.Update(now);
}

std::vector<LobbyEntry> LanLobbyService::Results() const {
    std::vector<LobbyEntry> out;
    for (const LanSession& s : browser_.Results()) out.push_back({s.address.ToString(), s.address, s.info, s.ping, s.compatible});
    return out;
}

} // namespace aether::net
