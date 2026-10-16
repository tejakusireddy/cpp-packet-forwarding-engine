#include "forwarding/forward.hpp"

#include <algorithm>

namespace forwarding {
namespace {
std::uint16_t checksum(std::span<const std::uint8_t> header) noexcept {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < header.size(); i += 2) {
        sum += (static_cast<std::uint32_t>(header[i]) << 8U) | header[i + 1];
    }
    while (sum >> 16U) {
        sum = (sum & 0xffffU) + (sum >> 16U);
    }
    return static_cast<std::uint16_t>(~sum);
}

bool extension_header(std::uint8_t protocol) noexcept {
    switch (protocol) {
    case 0: case 43: case 44: case 50: case 51: case 60:
    case 135: case 139: case 140: case 253: case 254:
        return true;
    default:
        return false;
    }
}
} // namespace

ForwardResult forward_packet(std::span<std::uint8_t> frame, const Fib& fib,
                             const NextHopTable& next_hops, std::uint64_t flow_hash) noexcept {
    const auto parsed = parse_packet(frame);
    if (const auto* error = std::get_if<ParseError>(&parsed)) {
        return *error;
    }
    const auto& packet = std::get<PacketView>(parsed);
    const bool ipv4 = packet.version == IpVersion::v4;
    if ((ipv4 && (packet.datagram[0] & 0x0fU) != 5) ||
        (!ipv4 && extension_header(packet.protocol))) {
        return ForwardError::unsupported_ip_options;
    }
    if (ipv4 && checksum(packet.datagram.first(20)) != 0) {
        return ForwardError::invalid_ipv4_checksum;
    }
    if (packet.hop_limit <= 1) {
        return ForwardError::hop_limit_expired;
    }
    const auto resolution = next_hops.resolve(fib, packet.version, packet.destination, flow_hash);
    if (const auto* error = std::get_if<ResolutionError>(&resolution)) {
        return *error;
    }
    const auto& resolved = std::get<ResolvedRoute>(resolution);

    // All fallible checks precede the first mutation.
    auto ip = frame.subspan(14);
    --ip[ipv4 ? 8 : 7];
    if (ipv4) {
        ip[10] = 0;
        ip[11] = 0;
        const auto updated = checksum(ip.first(20));
        ip[10] = static_cast<std::uint8_t>(updated >> 8U);
        ip[11] = static_cast<std::uint8_t>(updated & 0xffU);
    }
    std::copy(resolved.next_hop.destination_mac.begin(), resolved.next_hop.destination_mac.end(),
              frame.begin());
    std::copy(resolved.next_hop.source_mac.begin(), resolved.next_hop.source_mac.end(),
              frame.begin() + 6);
    return ForwardedPacket{resolved, 14 + packet.datagram.size()};
}

} // namespace forwarding
