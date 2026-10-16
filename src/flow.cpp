#include "forwarding/flow.hpp"

namespace forwarding {

std::uint64_t flow_hash(const PacketView& packet, std::uint64_t seed) noexcept {
    // Explicit byte order and fixed constants avoid implementation-defined
    // std::hash results. A configured seed allows independent ECMP domains.
    std::uint64_t hash = 14695981039346656037ULL ^ seed;
    const auto append = [&hash](std::uint8_t byte) {
        hash = (hash ^ byte) * 1099511628211ULL;
    };
    const bool ipv4 = packet.version == IpVersion::v4;
    append(ipv4 ? 4 : 6);
    const std::size_t address_size = ipv4 ? 4 : 16;
    for (std::size_t i = 0; i < address_size; ++i) {
        append(packet.source[i]);
    }
    for (std::size_t i = 0; i < address_size; ++i) {
        append(packet.destination[i]);
    }
    append(packet.protocol);

    std::size_t offset = 40;
    bool fragmented = false;
    if (ipv4) {
        // Bounds checks also make short manually constructed views harmless.
        if (packet.datagram.size() < 20) {
            return hash;
        }
        offset = static_cast<std::size_t>(packet.datagram[0] & 0x0fU) * 4;
        fragmented = (packet.datagram[6] & 0x3fU) != 0 || packet.datagram[7] != 0;
    }
    if (!fragmented && (packet.protocol == 6 || packet.protocol == 17) &&
        offset >= (ipv4 ? 20U : 40U) && packet.datagram.size() >= offset + 4) {
        for (std::size_t i = 0; i < 4; ++i) {
            append(packet.datagram[offset + i]);
        }
    }
    return hash;
}

} // namespace forwarding
