#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace forwarding {

enum class IpVersion { v4, v6 };
enum class ParseError {
    truncated_ethernet,
    unsupported_ether_type,
    truncated_ip,
    invalid_version,
    invalid_header_length,
    invalid_packet_length,
    unsupported_jumbogram,
};

// IPv4 addresses occupy the first four bytes; remaining bytes are zero.
struct PacketView {
    IpVersion version;
    std::array<std::uint8_t, 16> source{};
    std::array<std::uint8_t, 16> destination{};
    std::uint8_t hop_limit;
    // IPv6 next-header value is returned without traversing extension headers.
    std::uint8_t protocol;
    // Borrowed view of the complete IP datagram, excluding Ethernet padding.
    std::span<const std::uint8_t> datagram;
};

using ParseResult = std::variant<PacketView, ParseError>;

// Parses untagged Ethernet II frames. Does not verify checksums or mutate input.
// The caller must keep the input buffer alive while using a returned view.
[[nodiscard]] ParseResult parse_packet(std::span<const std::uint8_t> frame) noexcept;

} // namespace forwarding
