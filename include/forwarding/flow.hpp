#pragma once

#include "forwarding/packet.hpp"

namespace forwarding {

// Hashes an unmodified view returned by parse_packet. Directional and stable
// across platforms; excludes TTL, checksums, MAC addresses, and payload bytes.
// TCP/UDP ports are included only when present in a non-fragmented packet.
// All IPv4 fragments use addresses and protocol, including the first fragment.
// Other protocols and short transport payloads also use this three-field key.
// IPv6 extension headers are not traversed. This is not a cryptographic hash.
[[nodiscard]] std::uint64_t flow_hash(const PacketView& packet,
                                     std::uint64_t seed = 0) noexcept;

} // namespace forwarding
