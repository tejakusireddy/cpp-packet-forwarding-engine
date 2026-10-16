#include "forwarding/next_hop.hpp"

#include <cstdlib>
#include <iostream>

using namespace forwarding;

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

NextHop member(std::uint32_t id) {
    return NextHop{id, id + 10, {2, 0, 0, 0, 0, 1},
                   {2, 0, 0, 0, 1, static_cast<std::uint8_t>(id)}, true};
}

void errors_and_updates() {
    Fib fib;
    NextHopTable table;
    const IpAddress destination{192, 0, 2, 1};
    check(table.resolve(fib, IpVersion::v4, destination, 0) ==
              ResolutionResult{ResolutionError::no_route}, "missing route");
    check(fib.insert(IpVersion::v4, {}, 0, 0), "install default route");
    check(table.resolve(fib, IpVersion::v4, destination, 0) ==
              ResolutionResult{ResolutionError::missing_group}, "missing group");
    auto hop = member(0);
    check(table.upsert(0, std::span(&hop, 1)), "install single member group");
    const auto saved = table.resolve(fib, IpVersion::v4, destination, 42);
    check(saved == ResolutionResult{ResolvedRoute{FibMatch{0, 0}, hop}},
          "resolve complete adjacency");
    check(table.size() == 1, "group count");
    check(!table.upsert(0, {}), "reject empty group");
    const std::array duplicates{hop, hop};
    check(!table.upsert(0, duplicates), "reject duplicate IDs");
    check(table.resolve(fib, IpVersion::v4, destination, 42) == saved,
          "rejected update changed group");
    hop.available = false;
    check(table.upsert(0, std::span(&hop, 1)), "install unavailable member");
    check(table.resolve(fib, IpVersion::v4, destination, 42) ==
              ResolutionResult{ResolutionError::no_available_members}, "unavailable group");
    check(std::get<ResolvedRoute>(saved).next_hop.available, "result must own its adjacency");
    check(table.size() == 1, "replacement must not add a group");
    check(table.erase(0), "remove group");
    check(!table.erase(0) && table.size() == 0, "remove absent group");
    check(table.resolve(fib, IpVersion::v4, destination, 42) ==
              ResolutionResult{ResolutionError::missing_group}, "removed group still resolves");
}

void ecmp() {
    Fib fib;
    NextHopTable table;
    NextHopTable replica;
    std::array members{member(0), member(1), member(2), member(3)};
    check(fib.insert(IpVersion::v4, {}, 0, 7), "install ECMP route");
    check(table.upsert(7, members) && replica.upsert(7, members), "install ECMP groups");
    const auto first = table.resolve(fib, IpVersion::v4, {}, 0);
    check(std::get<ResolvedRoute>(first).next_hop.id == 3, "fixed selection vector");
    std::array<std::size_t, 4> counts{};
    for (std::uint64_t flow = 0; flow < 100000; ++flow) {
        const auto result = table.resolve(fib, IpVersion::v4, {}, flow);
        check(result == table.resolve(fib, IpVersion::v4, {}, flow), "flow affinity");
        check(result == replica.resolve(fib, IpVersion::v4, {}, flow), "table determinism");
        const auto& selected = std::get<ResolvedRoute>(result).next_hop;
        check(selected.id < counts.size(), "invalid member selected");
        check(selected == members[selected.id], "adjacency fields lost");
        ++counts[selected.id];
    }
    for (const auto count : counts) {
        check(count > 23750 && count < 26250, "uneven ECMP distribution");
    }

    members[1].available = false;
    members[3].available = false;
    check(table.upsert(7, members), "update availability");
    for (std::uint64_t flow = 0; flow < 1000; ++flow) {
        const auto result = table.resolve(fib, IpVersion::v4, {}, flow);
        const auto id = std::get<ResolvedRoute>(result).next_hop.id;
        check(id == 0 || id == 2, "selected unavailable member");
    }
    check(fib.insert(IpVersion::v4, {}, 32, 99), "install more-specific route");
    check(table.resolve(fib, IpVersion::v4, {}, 0) ==
              ResolutionResult{ResolutionError::missing_group},
          "must not fall back to default when selected group is missing");
}

void parser_integration() {
    for (const auto family : {IpVersion::v4, IpVersion::v6}) {
        const bool v4 = family == IpVersion::v4;
        std::array<std::uint8_t, 54> frame{};
        frame[12] = v4 ? 0x08 : 0x86;
        frame[13] = v4 ? 0 : 0xdd;
        frame[14] = v4 ? 0x45 : 0x60;
        if (v4) {
            frame[17] = 20;
            frame[30] = 192;
            frame[33] = 9;
        } else {
            frame[20] = 59;
            frame[38] = 0x20;
            frame[53] = 9;
        }
        const auto parsed = parse_packet(frame);
        check(std::holds_alternative<PacketView>(parsed), "integration packet parse");
        const auto& packet = std::get<PacketView>(parsed);
        Fib fib;
        NextHopTable table;
        const auto hop = member(11);
        const std::size_t length = v4 ? 32 : 128;
        check(fib.insert(family, packet.destination, length, 5), "install packet destination");
        check(table.upsert(5, std::span(&hop, 1)), "install packet adjacency");
        check(table.resolve(fib, packet.version, packet.destination, 123) ==
                  ResolutionResult{ResolvedRoute{FibMatch{5, length}, hop}},
              "parser to FIB to adjacency resolution");
    }
}
} // namespace

int main() {
    errors_and_updates();
    ecmp();
    parser_integration();
    std::cout << "All next-hop tests passed\n";
}
