#include "forwarding/forward.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace forwarding;

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// Fold carries after each word, independently of the production checksum loop.
std::uint16_t folded_sum(std::span<const std::uint8_t> bytes) {
    unsigned sum = 0;
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        sum += static_cast<unsigned>(bytes[i]) * 256 + bytes[i + 1];
        sum = (sum & 65535U) + (sum / 65536U);
    }
    return static_cast<std::uint16_t>(sum);
}

void repair_checksum(std::vector<std::uint8_t>& frame) {
    frame[24] = frame[25] = 0;
    const auto sum = static_cast<std::uint16_t>(~folded_sum(std::span(frame).subspan(14, 20)));
    frame[24] = static_cast<std::uint8_t>(sum >> 8U);
    frame[25] = static_cast<std::uint8_t>(sum & 255U);
}

std::vector<std::uint8_t> make_frame(bool ipv4, std::uint8_t ttl = 64) {
    std::vector<std::uint8_t> frame(ipv4 ? 38 : 58, 0);
    frame[12] = ipv4 ? 0x08 : 0x86;
    frame[13] = ipv4 ? 0 : 0xdd;
    frame[14] = ipv4 ? 0x45 : 0x60;
    if (ipv4) {
        frame[17] = 24;
        frame[22] = ttl;
        frame[23] = 17;
        frame[26] = 192;
        frame[29] = 1;
        frame[30] = 198;
        frame[31] = 51;
        frame[32] = 100;
        frame[33] = 2;
        repair_checksum(frame);
    } else {
        frame[19] = 4;
        frame[20] = 17;
        frame[21] = ttl;
        frame[22] = frame[38] = 0x20;
        frame[37] = 1;
        frame[53] = 2;
    }
    std::fill(frame.end() - 4, frame.end(), 0xab);
    frame.resize(80, 0xcc);
    return frame;
}

const NextHop hop{9, 17, {2, 1, 2, 3, 4, 5}, {2, 6, 7, 8, 9, 10}, true};

void success() {
    Fib fib;
    NextHopTable table;
    check(fib.insert(IpVersion::v4, {}, 0, 1), "install IPv4 route");
    check(fib.insert(IpVersion::v6, {}, 0, 1), "install IPv6 route");
    check(table.upsert(1, std::span(&hop, 1)), "install adjacency");
    for (const bool ipv4 : {true, false}) {
        for (unsigned ttl = 2; ttl <= 255; ++ttl) {
            auto frame = make_frame(ipv4, static_cast<std::uint8_t>(ttl));
            const auto before = frame;
            const auto result = forward_packet(frame, fib, table, 123);
            check(std::holds_alternative<ForwardedPacket>(result), "valid frame rejected");
            const auto& forwarded = std::get<ForwardedPacket>(result);
            check(forwarded.resolved == ResolvedRoute{FibMatch{1, 0}, hop}, "wrong output adjacency");
            check(forwarded.frame_length == (ipv4 ? 38 : 58), "wrong transmit length");
            check(std::equal(hop.destination_mac.begin(), hop.destination_mac.end(), frame.begin()),
                  "destination MAC rewrite");
            check(std::equal(hop.source_mac.begin(), hop.source_mac.end(), frame.begin() + 6),
                  "source MAC rewrite");
            check(frame[ipv4 ? 22 : 21] == ttl - 1, "hop decrement");
            if (ipv4) {
                check(folded_sum(std::span(frame).subspan(14, 20)) == 0xffff,
                      "invalid updated IPv4 checksum");
            }
            for (std::size_t i = 12; i < frame.size(); ++i) {
                if (i == (ipv4 ? 22U : 21U) || (ipv4 && (i == 24 || i == 25))) {
                    continue;
                }
                check(frame[i] == before[i], "unexpected header, payload, or padding mutation");
            }
        }
    }
}

template <typename Error>
void rejected(std::vector<std::uint8_t> frame, const Fib& fib,
              const NextHopTable& table, Error error) {
    const auto before = frame;
    const auto result = forward_packet(frame, fib, table, 0);
    check(std::holds_alternative<Error>(result) && std::get<Error>(result) == error,
          "unexpected forwarding error");
    check(frame == before, "rejected frame was mutated");
}

void failures() {
    Fib fib;
    NextHopTable table;
    for (const bool ipv4 : {true, false}) {
        const auto family = ipv4 ? IpVersion::v4 : IpVersion::v6;
        const auto frame = make_frame(ipv4);
        const std::size_t wire_size = ipv4 ? 38 : 58;
        for (std::size_t length = 0; length < wire_size; ++length) {
            auto truncated = frame;
            truncated.resize(length);
            const auto parsed = parse_packet(truncated);
            check(std::holds_alternative<ParseError>(parsed), "truncation fixture");
            rejected(truncated, fib, table, std::get<ParseError>(parsed));
        }
        for (const auto ttl : {0, 1}) {
            rejected(make_frame(ipv4, static_cast<std::uint8_t>(ttl)), fib, table,
                     ForwardError::hop_limit_expired);
        }
        rejected(frame, fib, table, ResolutionError::no_route);
        check(fib.insert(family, {}, 0, 1), "install unresolved route");
        rejected(frame, fib, table, ResolutionError::missing_group);
    }
    auto unavailable = hop;
    unavailable.available = false;
    check(table.upsert(1, std::span(&unavailable, 1)), "install unavailable adjacency");
    for (const bool ipv4 : {true, false}) {
        rejected(make_frame(ipv4), fib, table, ResolutionError::no_available_members);
    }
    check(table.upsert(1, std::span(&hop, 1)), "install usable adjacency");
    auto corrupt = make_frame(true);
    corrupt[29] ^= 1;
    rejected(corrupt, fib, table, ForwardError::invalid_ipv4_checksum);
    auto options = make_frame(true);
    options[14] = 0x46;
    rejected(options, fib, table, ForwardError::unsupported_ip_options);
    for (const auto protocol : {0, 43, 44, 50, 51, 60, 135, 139, 140, 253, 254}) {
        auto extension = make_frame(false);
        extension[20] = static_cast<std::uint8_t>(protocol);
        rejected(extension, fib, table, ForwardError::unsupported_ip_options);
    }
}
} // namespace

int main() {
    success();
    failures();
    std::cout << "All forwarding tests passed\n";
}
