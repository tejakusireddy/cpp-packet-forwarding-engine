#pragma once

#include "forwarding/fib.hpp"

#include <span>
#include <unordered_map>
#include <variant>
#include <vector>

namespace forwarding {

using MacAddress = std::array<std::uint8_t, 6>;

// A resolved Ethernet adjacency. Interface and member IDs are opaque values.
// The control plane supplies MAC addresses and availability; no ARP/ND runs here.
struct NextHop {
    std::uint32_t id;
    std::uint32_t interface_id;
    MacAddress source_mac;
    MacAddress destination_mac;
    bool available = true;
    bool operator==(const NextHop&) const = default;
};

struct ResolvedRoute {
    FibMatch route;
    NextHop next_hop;
    bool operator==(const ResolvedRoute&) const = default;
};

enum class ResolutionError { no_route, missing_group, no_available_members };
using ResolutionResult = std::variant<ResolvedRoute, ResolutionError>;

// Concurrent reads require the FIB and group table to remain unchanged.
class NextHopTable {
public:
    // Copies members in supplied order. Empty groups and duplicate member IDs
    // are rejected without changing an existing group. Zero IDs are valid.
    [[nodiscard]] bool upsert(std::uint32_t group_id, std::span<const NextHop> members);
    [[nodiscard]] bool erase(std::uint32_t group_id);
    [[nodiscard]] std::size_t size() const noexcept { return groups_.size(); }
    [[nodiscard]] bool contains(std::uint32_t group_id) const { return groups_.contains(group_id); }

    // The caller supplies a stable flow hash. Mixes it and selects an equal-cost
    // available member. Results are copied and remain valid after table updates.
    [[nodiscard]] ResolutionResult resolve(const Fib& fib, IpVersion family,
                                            const IpAddress& destination,
                                            std::uint64_t flow_hash) const noexcept;

private:
    struct Group {
        std::vector<NextHop> active;
    };
    std::unordered_map<std::uint32_t, Group> groups_;
};

} // namespace forwarding
