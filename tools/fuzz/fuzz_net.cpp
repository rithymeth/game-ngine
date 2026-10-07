// Fuzz target: the session discovery wire decoder over arbitrary datagrams.
#include "fuzz_common.h"

#include "aether/net/session.h"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    aether::net::SessionInfo info;
    if (aether::net::DecodeSessionInfo(std::span<const aether::u8>(data, size), info)) {
        const std::vector<aether::u8> encoded = aether::net::EncodeSessionInfo(info);
        aether::net::SessionInfo round_trip;
        (void)aether::net::DecodeSessionInfo(encoded, round_trip);
    }
    return 0;
}
