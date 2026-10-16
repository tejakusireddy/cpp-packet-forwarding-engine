#include "forwarding/packet.hpp"

#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

using forwarding::PacketView;
using forwarding::ParseError;
using forwarding::parse_packet;

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::vector<std::uint8_t> ipv4() {
    std::vector<std::uint8_t> frame(38, 0);
    frame[12] = 0x08;
    frame[14] = 0x45;
    frame[17] = 24;
    frame[22] = 64;
    frame[23] = 17;
    frame[26] = 192;
    frame[27] = 0;
    frame[28] = 2;
    frame[29] = 1;
    frame[30] = 198;
    frame[31] = 51;
    frame[32] = 100;
    frame[33] = 2;
    return frame;
}

std::vector<std::uint8_t> ipv6() {
    std::vector<std::uint8_t> frame(58, 0);
    frame[12] = 0x86;
    frame[13] = 0xdd;
    frame[14] = 0x60;
    frame[19] = 4;
    frame[20] = 17;
    frame[21] = 32;
    frame[22] = 0x20;
    frame[23] = 0x01;
    frame[37] = 1;
    frame[38] = 0x20;
    frame[39] = 0x01;
    frame[53] = 2;
    return frame;
}

void expect_error(const std::vector<std::uint8_t>& frame, ParseError error) {
    const auto result = parse_packet(frame);
    check(std::holds_alternative<ParseError>(result), "expected a parse error");
    check(std::get<ParseError>(result) == error, "unexpected parse error");
}

void valid_packets() {
    for (const bool v4 : {true, false}) {
        auto frame = v4 ? ipv4() : ipv6();
        // Intentionally unaligned input, with Ethernet padding after the datagram.
        frame.insert(frame.begin(), 0);
        frame.resize(frame.size() + 16, 0xff);
        const auto result = parse_packet(std::span(frame).subspan(1));
        check(std::holds_alternative<PacketView>(result), "valid packet rejected");
        const auto& packet = std::get<PacketView>(result);
        check(packet.version == (v4 ? forwarding::IpVersion::v4 : forwarding::IpVersion::v6),
              "wrong IP version");
        check(packet.hop_limit == (v4 ? 64 : 32), "wrong hop limit");
        check(packet.protocol == 17, "wrong protocol");
        check(packet.source[0] == (v4 ? 192 : 0x20), "wrong source address");
        check(packet.destination[v4 ? 3 : 15] == 2, "wrong destination address");
        check(packet.datagram.size() == (v4 ? 24 : 44), "padding included in datagram");
        check(packet.datagram.data() == frame.data() + 15, "datagram is not borrowed");
        if (v4) {
            for (std::size_t i = 4; i < 16; ++i) {
                check(packet.source[i] == 0 && packet.destination[i] == 0,
                      "IPv4 address tail is not zero");
            }
        }
    }

    auto frame = ipv4();
    frame[14] = 0x46;
    check(std::holds_alternative<PacketView>(parse_packet(frame)), "IPv4 options rejected");
    frame = ipv6();
    frame.resize(54);
    frame[19] = 0;
    frame[20] = 59;
    check(std::holds_alternative<PacketView>(parse_packet(frame)), "empty IPv6 packet rejected");
}

void malformed_packets() {
    for (const auto& frame : {ipv4(), ipv6()}) {
        for (std::size_t length = 0; length < frame.size(); ++length) {
            check(std::holds_alternative<ParseError>(
                      parse_packet(std::span(frame).first(length))),
                  "truncated packet accepted");
        }
    }
    auto frame = ipv4();
    frame[13] = 6;
    expect_error(frame, ParseError::unsupported_ether_type);
    frame[12] = 0x81;
    frame[13] = 0;
    expect_error(frame, ParseError::unsupported_ether_type);
    frame = ipv4();
    frame[14] = 0x65;
    expect_error(frame, ParseError::invalid_version);
    frame[14] = 0x44;
    expect_error(frame, ParseError::invalid_header_length);
    frame[14] = 0x4f;
    expect_error(frame, ParseError::truncated_ip);
    frame[14] = 0x46;
    frame[17] = 20;
    expect_error(frame, ParseError::invalid_packet_length);
    frame = ipv6();
    frame[14] = 0x40;
    expect_error(frame, ParseError::invalid_version);
    frame[14] = 0x60;
    frame[18] = 0xff;
    expect_error(frame, ParseError::truncated_ip);
    frame[18] = 0;
    frame[19] = 0;
    frame[20] = 0;
    expect_error(frame, ParseError::unsupported_jumbogram);
}

void randomized_bounds() {
    std::mt19937 random(42);
    std::vector<std::uint8_t> frame;
    for (unsigned iteration = 0; iteration < 20000; ++iteration) {
        frame.resize(random() % 2048);
        for (auto& byte : frame) {
            byte = static_cast<std::uint8_t>(random());
        }
        if (frame.size() >= 14) {
            const bool v4 = iteration % 2 == 0;
            frame[12] = v4 ? 0x08 : 0x86;
            frame[13] = v4 ? 0 : 0xdd;
        }
        const auto result = parse_packet(frame);
        if (const auto* packet = std::get_if<PacketView>(&result)) {
            check(packet->datagram.data() == frame.data() + 14, "invalid datagram start");
            check(packet->datagram.size() <= frame.size() - 14, "datagram exceeds input");
        }
    }
}
} // namespace

int main() {
    valid_packets();
    malformed_packets();
    randomized_bounds();
    std::cout << "All packet parser tests passed\n";
}
