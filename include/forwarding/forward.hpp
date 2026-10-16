#pragma once

#include "forwarding/next_hop.hpp"

namespace forwarding {

enum class ForwardError { invalid_ipv4_checksum, hop_limit_expired, unsupported_ip_options };

struct ForwardedPacket {
    ResolvedRoute resolved;
    // Bytes through the end of the IP datagram. Link padding is left untouched;
    // the eventual transmit backend must provide Ethernet minimum-frame padding.
    std::size_t frame_length;
};

using ForwardResult = std::variant<ForwardedPacket, ParseError, ResolutionError, ForwardError>;

// Prepares a frame in place; performs no I/O or allocations. Any error leaves
// every input byte unchanged. The caller supplies the stable ECMP flow hash.
// IPv4 options and IPv6 extension headers require a separate processing path.
[[nodiscard]] ForwardResult forward_packet(std::span<std::uint8_t> frame,
                                           const Fib& fib, const NextHopTable& next_hops,
                                           std::uint64_t flow_hash) noexcept;

} // namespace forwarding
