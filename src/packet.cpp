#include "forwarding/packet.hpp"

#include <algorithm>

namespace forwarding {
namespace {
constexpr std::size_t ethernet_size = 14;

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>((static_cast<unsigned>(bytes[offset]) << 8U) |
                                      bytes[offset + 1]);
}
} // namespace

ParseResult parse_packet(std::span<const std::uint8_t> frame) noexcept {
    if (frame.size() < ethernet_size) {
        return ParseError::truncated_ethernet;
    }
    const auto ether_type = read_u16(frame, 12);
    if (ether_type != 0x0800 && ether_type != 0x86dd) {
        return ParseError::unsupported_ether_type;
    }
    const auto ip = frame.subspan(ethernet_size);
    const bool ipv4 = ether_type == 0x0800;
    const std::size_t minimum_size = ipv4 ? 20 : 40;
    if (ip.size() < minimum_size) {
        return ParseError::truncated_ip;
    }
    if ((ip[0] >> 4U) != (ipv4 ? 4 : 6)) {
        return ParseError::invalid_version;
    }

    PacketView packet{};
    if (ipv4) {
        const auto header_size = static_cast<std::size_t>(ip[0] & 0x0fU) * 4;
        if (header_size < minimum_size) {
            return ParseError::invalid_header_length;
        }
        if (header_size > ip.size()) {
            return ParseError::truncated_ip;
        }
        const auto length = read_u16(ip, 2);
        if (length < header_size) {
            return ParseError::invalid_packet_length;
        }
        if (length > ip.size()) {
            return ParseError::truncated_ip;
        }
        packet.version = IpVersion::v4;
        std::copy_n(ip.begin() + 12, 4, packet.source.begin());
        std::copy_n(ip.begin() + 16, 4, packet.destination.begin());
        packet.hop_limit = ip[8];
        packet.protocol = ip[9];
        packet.datagram = ip.first(length);
    } else {
        const auto payload_size = read_u16(ip, 4);
        // A zero payload with Hop-by-Hop options may encode a jumbogram.
        if (payload_size == 0 && ip[6] == 0) {
            return ParseError::unsupported_jumbogram;
        }
        const auto length = minimum_size + payload_size;
        if (length > ip.size()) {
            return ParseError::truncated_ip;
        }
        packet.version = IpVersion::v6;
        std::copy_n(ip.begin() + 8, 16, packet.source.begin());
        std::copy_n(ip.begin() + 24, 16, packet.destination.begin());
        packet.hop_limit = ip[7];
        packet.protocol = ip[6];
        packet.datagram = ip.first(length);
    }
    return packet;
}

} // namespace forwarding
