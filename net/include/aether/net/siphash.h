#pragma once

#include "aether/core/base.h"

#include <array>
#include <span>

namespace aether::net {

// SipHash-2-4 (Aumasson & Bernstein, "SipHash: a fast short-input PRF", 2012), written from the
// reference implementation (github.com/veorq/SipHash) and checked against its published test
// vectors in tests/test_net_production.cpp. A keyed 64-bit PRF used as the MAC for handshake
// cookies, datagrams and join tokens (M8.9). It is a MAC, not a cipher: nothing is encrypted.
using SipKey = std::array<u8, 16>;
u64 SipHash24(const SipKey& key, std::span<const u8> data);

// Constant-time equality for tags.
bool TagEqual(u64 a, u64 b);

} // namespace aether::net
