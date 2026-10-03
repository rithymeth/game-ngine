#include "aether/net/socket.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace aether::net {

std::optional<Address> Address::Parse(const std::string& text) {
    unsigned a = 0, b = 0, c = 0, d = 0, port = 0;
    char tail = 0;
    if (std::sscanf(text.c_str(), "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &port, &tail) != 5) return std::nullopt;
    if (a > 255 || b > 255 || c > 255 || d > 255 || port > 65535) return std::nullopt;
    return Address{(a << 24) | (b << 16) | (c << 8) | d, static_cast<u16>(port)};
}

std::string Address::ToString() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (ip >> 24) & 255u, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u, static_cast<unsigned>(port));
    return buf;
}

// --- UDP ------------------------------------------------------------------------------------------------

namespace {
#ifdef _WIN32
bool StartSockets() {
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}
using Native = SOCKET;
void CloseNative(Native s) { closesocket(s); }
#else
bool StartSockets() { return true; }
using Native = int;
void CloseNative(Native s) { close(s); }
#endif
Native ToNative(i64 h) { return static_cast<Native>(h); }
} // namespace

UdpSocket::~UdpSocket() { Close(); }

bool UdpSocket::Open(u16 port, std::string* error) {
    Close();
    const auto fail = [&](const char* what) {
        if (error != nullptr) *error = what;
        Close();
        return false;
    };
    if (!StartSockets()) return fail("the socket library didn't start");
    const Native s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return fail("couldn't make a UDP socket");
#else
    if (s < 0) return fail("couldn't make a UDP socket");
#endif
    handle_ = static_cast<i64>(s);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) return fail("couldn't bind the port (in use?)");
    const int yes = 1; // so LAN discovery can broadcast
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(s, FIONBIO, &nonblocking) != 0) return fail("couldn't make the socket non-blocking");
#else
    if (fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) != 0) return fail("couldn't make the socket non-blocking");
#endif
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len);
    port_ = ntohs(bound.sin_port);
    return true;
}

void UdpSocket::Close() {
    if (handle_ != kInvalid) CloseNative(ToNative(handle_));
    handle_ = kInvalid;
    port_ = 0;
}

bool UdpSocket::Send(const Address& to, std::span<const u8> data) {
    if (handle_ == kInvalid) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(to.ip);
    addr.sin_port = htons(to.port);
    const auto sent = sendto(ToNative(handle_), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                             reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    return sent >= 0 && static_cast<usize>(sent) == data.size();
}

bool UdpSocket::Receive(Address& from, std::vector<u8>& data) {
    if (handle_ == kInvalid) return false;
    data.resize(65536);
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    const auto got = recvfrom(ToNative(handle_), reinterpret_cast<char*>(data.data()), static_cast<int>(data.size()), 0, reinterpret_cast<sockaddr*>(&addr), &len);
    if (got <= 0) {
        data.clear();
        return false;
    }
    data.resize(static_cast<usize>(got));
    from = {ntohl(addr.sin_addr.s_addr), ntohs(addr.sin_port)};
    return true;
}

Address UdpSocket::LocalAddress() const { return Address::Loopback(port_); }

// --- Loopback ---------------------------------------------------------------------------------------------

class LoopbackSocket final : public DatagramSocket {
public:
    LoopbackSocket(LoopbackNetwork& net, Address address) : net_(net), address_(address) {}
    ~LoopbackSocket() override { net_.Close(address_); }
    bool Send(const Address& to, std::span<const u8> data) override {
        net_.Post(address_, to, data);
        return true;
    }
    bool Receive(Address& from, std::vector<u8>& data) override { return net_.Take(address_, from, data); }
    Address LocalAddress() const override { return address_; }

private:
    LoopbackNetwork& net_;
    Address address_;
};

std::unique_ptr<DatagramSocket> LoopbackNetwork::Open(u16 port) { return OpenAt(0x7F000001u, port); }

std::unique_ptr<DatagramSocket> LoopbackNetwork::OpenAt(u32 ip, u16 port) {
    if (port == 0) {
        while (open_.count(Address{ip, next_port_}) != 0) ++next_port_;
        port = next_port_++;
    }
    const Address address{ip, port};
    if (open_.count(address) != 0) return nullptr;
    open_[address] = true;
    return std::make_unique<LoopbackSocket>(*this, address);
}

void LoopbackNetwork::Close(const Address& address) {
    open_.erase(address);
    std::erase_if(in_flight_, [&](const Datagram& d) { return d.to == address; });
}

void LoopbackNetwork::Post(const Address& from, const Address& to, std::span<const u8> data) {
    ++sent_;
    if (to.IsBroadcast()) {
        std::vector<Address> targets;
        for (const auto& [address, open] : open_)
            if (address.port == to.port && !(address == from)) targets.push_back(address);
        if (targets.empty()) ++dropped_;
        for (const Address& t : targets) Deliver(from, t, data);
        return;
    }
    if (open_.count(to) == 0) { // nobody there: gone, as with UDP
        ++dropped_;
        return;
    }
    Deliver(from, to, data);
}

void LoopbackNetwork::Deliver(const Address& from, const Address& to, std::span<const u8> data) {
    std::uniform_real_distribution<f64> u(0.0, 1.0);
    if (u(rng_) < conditions.loss) {
        ++dropped_;
        return;
    }
    const int copies = u(rng_) < conditions.duplicate ? 2 : 1;
    for (int i = 0; i < copies; ++i) {
        const f64 delay = conditions.latency + conditions.jitter * u(rng_);
        in_flight_.push_back({now_ + delay, order_++, from, to, std::vector<u8>(data.begin(), data.end())});
    }
}

bool LoopbackNetwork::Take(const Address& to, Address& from, std::vector<u8>& data) {
    // The earliest due datagram for this address (ties: in sending order).
    auto best = in_flight_.end();
    for (auto it = in_flight_.begin(); it != in_flight_.end(); ++it) {
        if (!(it->to == to) || it->deliver_at > now_ + 1e-12) continue;
        if (best == in_flight_.end() || it->deliver_at < best->deliver_at || (it->deliver_at == best->deliver_at && it->order < best->order)) best = it;
    }
    if (best == in_flight_.end()) return false;
    from = best->from;
    data = std::move(best->data);
    in_flight_.erase(best);
    return true;
}

} // namespace aether::net
