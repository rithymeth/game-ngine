#include "aether/net/udp.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace aether::net {

namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kBadSocket = INVALID_SOCKET;
void CloseSocket(SocketHandle s) { closesocket(s); }
int LastError() { return WSAGetLastError(); }
bool WouldBlock(int e) { return e == WSAEWOULDBLOCK; }
bool Transient(int e) { return e == WSAECONNRESET || e == WSAEMSGSIZE || e == WSAENETRESET; } // an ICMP error for an earlier send

// Winsock needs a startup/cleanup pair around its use; keep it for the process.
void EnsureSockets() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
#else
using SocketHandle = int;
constexpr SocketHandle kBadSocket = -1;
void CloseSocket(SocketHandle s) { close(s); }
int LastError() { return errno; }
bool WouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
bool Transient(int e) { return e == ECONNREFUSED || e == ECONNRESET || e == EINTR; }
void EnsureSockets() {}
#endif

u64 Key(u32 ip, u16 port) { return (static_cast<u64>(ip) << 16) | port; }

std::string Dotted(u32 ip_network_order) {
    in_addr a;
    a.s_addr = ip_network_order;
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
}

} // namespace

std::unique_ptr<UdpTransport> UdpTransport::Create(const UdpConfig& config, std::string* error) {
    auto fail = [&](const char* what) {
        if (error != nullptr) *error = std::string(what) + " (error " + std::to_string(LastError()) + ")";
        return std::unique_ptr<UdpTransport>();
    };
    EnsureSockets();
    SocketHandle s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBadSocket) return fail("could not create a UDP socket");

#if defined(_WIN32)
    u_long nonblocking = 1;
    if (ioctlsocket(s, FIONBIO, &nonblocking) != 0) {
#else
    if (fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) != 0) {
#endif
        CloseSocket(s);
        return fail("could not make the socket non-blocking");
    }
    if (config.allow_broadcast) {
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    }
#if defined(_WIN32)
    // Don't let ICMP "port unreachable" replies surface as receive errors (they would hide real datagrams).
    BOOL no_reset = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &no_reset, sizeof(no_reset), nullptr, 0, &returned, nullptr, nullptr);
#endif
    if (config.allow_broadcast && config.port != 0) {
        int reuse = 1; // several programs on one machine may listen for discovery
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(config.port);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int code = LastError();
        CloseSocket(s);
        if (error != nullptr) *error = "could not bind port " + std::to_string(config.port) + " (error " + std::to_string(code) + ")";
        return nullptr;
    }
    socklen_t len = sizeof(addr);
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);

    std::unique_ptr<UdpTransport> t(new UdpTransport());
    t->socket_ = static_cast<std::intptr_t>(s);
    t->port_ = ntohs(addr.sin_port);
    t->max_addresses_ = std::max<usize>(config.max_addresses, 2);
    return t;
}

UdpTransport::~UdpTransport() {
    if (socket_ != -1) CloseSocket(static_cast<SocketHandle>(socket_));
}

void UdpTransport::Evict() {
    auto oldest = std::min_element(by_id_.begin(), by_id_.end(), [](const auto& a, const auto& b) { return a.second.last_used < b.second.last_used; });
    if (oldest == by_id_.end()) return;
    by_key_.erase(Key(oldest->second.ip, oldest->second.port));
    by_id_.erase(oldest);
}

NetAddress UdpTransport::Handle(u32 ip, u16 port) {
    auto it = by_key_.find(Key(ip, port));
    if (it != by_key_.end()) {
        by_id_[it->second].last_used = ++clock_;
        return it->second;
    }
    if (by_id_.size() >= max_addresses_) Evict();
    const NetAddress id = next_id_++;
    by_id_[id] = {ip, port, ++clock_};
    by_key_[Key(ip, port)] = id;
    return id;
}

NetAddress UdpTransport::AddressFor(const std::string& host, u16 port) {
    if (port == 0 || host.empty()) return 0;
    in_addr a{};
    if (inet_pton(AF_INET, host.c_str(), &a) == 1) return Handle(a.s_addr, port);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) return 0;
    const u32 ip = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(result);
    return Handle(ip, port);
}

NetAddress UdpTransport::AddressWithPort(NetAddress address, u16 port) {
    auto it = by_id_.find(address);
    return it == by_id_.end() || port == 0 ? 0 : Handle(it->second.ip, port);
}

bool UdpTransport::Describe(NetAddress address, std::string& host, u16& port) const {
    auto it = by_id_.find(address);
    if (it == by_id_.end()) return false;
    host = Dotted(it->second.ip);
    port = it->second.port;
    return true;
}

bool UdpTransport::Send(NetAddress to, std::span<const u8> data) {
    auto it = by_id_.find(to);
    if (it == by_id_.end() || data.size() > MaxDatagramSize()) return false;
    it->second.last_used = ++clock_;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = it->second.ip;
    addr.sin_port = htons(it->second.port);
    const int sent = static_cast<int>(sendto(static_cast<SocketHandle>(socket_), reinterpret_cast<const char*>(data.data()),
                                             static_cast<int>(data.size()), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
    return sent == static_cast<int>(data.size());
}

bool UdpTransport::Broadcast(u16 port, std::span<const u8> data) {
    if (port == 0 || data.size() > MaxDatagramSize()) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    addr.sin_port = htons(port);
    const int sent = static_cast<int>(sendto(static_cast<SocketHandle>(socket_), reinterpret_cast<const char*>(data.data()),
                                             static_cast<int>(data.size()), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
    return sent == static_cast<int>(data.size());
}

bool UdpTransport::Receive(Datagram& out) {
    u8 buffer[2048];
    for (;;) {
        sockaddr_in from{};
        socklen_t len = sizeof(from);
        const int n = static_cast<int>(recvfrom(static_cast<SocketHandle>(socket_), reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                                                reinterpret_cast<sockaddr*>(&from), &len));
        if (n < 0) {
            const int e = LastError();
            if (WouldBlock(e)) return false;
            if (Transient(e)) continue; // an error about an earlier send, not a datagram
            return false;
        }
        if (static_cast<usize>(n) > MaxDatagramSize()) continue; // bigger than any sender here would send
        out.from = Handle(from.sin_addr.s_addr, ntohs(from.sin_port));
        out.data.assign(buffer, buffer + n);
        return true;
    }
}

} // namespace aether::net
