#include "forwarding/flow.hpp"
#include "forwarding/forward.hpp"

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

std::vector<std::uint8_t> frame(bool v4, std::uint8_t protocol) {
    std::vector<std::uint8_t> bytes(v4 ? 46 : 66, 0);
    bytes[12] = v4 ? 8 : 0x86;
    bytes[13] = v4 ? 0 : 0xdd;
    bytes[14] = v4 ? 0x45 : 0x60;
    if (v4) {
        bytes[17] = 32;
        bytes[22] = 64;
        bytes[23] = protocol;
        bytes[26] = 192;
        bytes[29] = 1;
        bytes[30] = 198;
        bytes[33] = 2;
    } else {
        bytes[19] = 12;
        bytes[20] = protocol;
        bytes[21] = 64;
        bytes[22] = bytes[38] = 0x20;
        bytes[37] = 1;
        bytes[53] = 2;
    }
    const std::size_t ports = v4 ? 34 : 54;
    bytes[ports] = 0x12;
    bytes[ports + 1] = 0x34;
    bytes[ports + 2] = 0x56;
    bytes[ports + 3] = 0x78;
    return bytes;
}

std::uint64_t hash(const std::vector<std::uint8_t>& bytes, std::uint64_t seed = 0) {
    const auto parsed = parse_packet(bytes);
    check(std::holds_alternative<PacketView>(parsed), "parse flow fixture");
    return flow_hash(std::get<PacketView>(parsed), seed);
}

void fields() {
    check(hash(frame(true, 17)) == 0x3ed16bd4895817c3ULL, "fixed IPv4 UDP hash vector");
    for (const bool v4 : {true, false}) {
        for (const auto protocol : {6, 17}) {
            const auto bytes = frame(v4, static_cast<std::uint8_t>(protocol));
            const auto expected = hash(bytes);
            check(hash(bytes) == expected, "repeatability");
            check(hash(bytes, 99) != expected, "seed ignored");
            const std::size_t ports = v4 ? 34 : 54;
            for (const auto offset : {v4 ? 26U : 22U, v4 ? 33U : 53U,
                                      static_cast<unsigned>(ports),
                                      static_cast<unsigned>(ports + 3)}) {
                auto changed = bytes;
                changed[offset] ^= 1;
                check(hash(changed) != expected, "flow field ignored");
            }
            auto changed = bytes;
            changed[v4 ? 23 : 20] = protocol == 6 ? 17 : 6;
            check(hash(changed) != expected, "protocol ignored");
            changed = bytes;
            changed[0] = changed[6] = 0xff;
            changed[v4 ? 22 : 21] = 2;
            if (v4) {
                changed[24] = 0xab;
                changed[25] = 0xcd;
                changed[20] = 0x40; // Don't-fragment bit is not fragmentation.
            }
            changed.back() = 0xef;
            changed.resize(changed.size() + 10, 0xff);
            check(hash(changed) == expected, "mutable or non-flow bytes affect hash");

            // Ports are included only if all four bytes fit inside the IP length.
            for (std::size_t length = 0; length < 4; ++length) {
                changed = bytes;
                changed.resize(ports + length);
                changed[v4 ? 17 : 19] = static_cast<std::uint8_t>((v4 ? 20 : 0) + length);
                const auto short_hash = hash(changed);
                if (length > 0) {
                    changed.back() ^= 0xff;
                    check(hash(changed) == short_hash, "short ports must use fallback key");
                }
            }
        }
        auto opaque = frame(v4, 1);
        const auto expected = hash(opaque);
        opaque[v4 ? 34 : 54] ^= 0xff;
        check(hash(opaque) == expected, "non-TCP/UDP payload affects hash");
    }
}

void fragments_and_options() {
    auto first = frame(true, 17);
    first[20] = 0x20; // More fragments, zero fragment offset.
    const auto expected = hash(first);
    auto later = first;
    later[20] = 0;
    later[21] = 1; // Last fragment, nonzero offset.
    later[34] ^= 0xff;
    later[35] ^= 0xff;
    check(hash(later) == expected, "last fragment follows first fragment");
    later[20] = 0x20;
    later[21] = 255;
    check(hash(later) == expected, "middle fragment follows first fragment");

    auto options = frame(true, 17);
    const auto plain_hash = hash(options);
    options.insert(options.begin() + 34, 4, 0);
    options[14] = 0x46;
    options[17] = 36;
    check(hash(options) == plain_hash, "IPv4 options shift port offset incorrectly");
    options[34] = 1;
    check(hash(options) == plain_hash, "IPv4 option bytes affect hash");

    auto extension = frame(false, 44);
    const auto extension_hash = hash(extension);
    extension[54] ^= 0xff;
    check(hash(extension) == extension_hash, "extension bytes treated as transport ports");
}

void integration() {
    Fib fib;
    NextHopTable table;
    const std::array members{
        NextHop{1, 11, {2, 0, 0, 0, 0, 1}, {2, 0, 0, 0, 1, 1}},
        NextHop{2, 12, {2, 0, 0, 0, 0, 2}, {2, 0, 0, 0, 1, 2}}};
    check(fib.insert(IpVersion::v6, {}, 0, 1), "install integration route");
    check(table.upsert(1, members), "install integration group");
    auto automatic = frame(false, 17);
    auto explicit_hash = automatic;
    const auto expected = forward_packet(explicit_hash, fib, table, hash(explicit_hash));
    const auto actual = forward_packet(automatic, fib, table);
    check(std::holds_alternative<ForwardedPacket>(actual) &&
          std::holds_alternative<ForwardedPacket>(expected), "automatic forwarding failed");
    check(std::get<ForwardedPacket>(actual).resolved == std::get<ForwardedPacket>(expected).resolved,
          "automatic forwarding selected different adjacency");
    check(automatic == explicit_hash, "automatic forwarding produced different frame");
    auto bad = frame(false, 17);
    bad[21] = 1;
    const auto before = bad;
    const auto rejected = forward_packet(bad, fib, table);
    check(std::holds_alternative<ForwardError>(rejected) && bad == before,
          "automatic forwarding mutated rejected frame");
}
} // namespace

int main() {
    fields();
    fragments_and_options();
    integration();
    std::cout << "All flow hash tests passed\n";
}
