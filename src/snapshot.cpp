#include "forwarding/snapshot.hpp"

#include <stdexcept>
#include <unordered_set>

namespace forwarding {

RoutingSnapshot::RoutingSnapshot(std::uint64_t version, std::span<const RouteEntry> routes,
                                 std::span<const NextHopGroup> groups)
    : version_(version) {
    std::unordered_set<std::uint32_t> group_ids;
    for (const auto& group : groups) {
        if (!group_ids.insert(group.id).second || !next_hops_.upsert(group.id, group.members)) {
            throw std::invalid_argument("Invalid or duplicate next-hop group");
        }
    }
    for (const auto& route : routes) {
        if (!group_ids.contains(route.next_hop_group)) {
            throw std::invalid_argument("Route references a missing next-hop group");
        }
        if (!fib_.insert(route.family, route.prefix, route.prefix_length, route.next_hop_group)) {
            throw std::invalid_argument("Invalid route prefix");
        }
    }
}

SnapshotStore::SnapshotStore()
    : current_(std::make_shared<const RoutingSnapshot>(
          0, std::span<const RouteEntry>{}, std::span<const NextHopGroup>{})) {}

std::shared_ptr<const RoutingSnapshot> SnapshotStore::acquire() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    return current_.load(std::memory_order_acquire);
#else
    return std::atomic_load_explicit(&current_, std::memory_order_acquire);
#endif
}

bool SnapshotStore::publish(std::shared_ptr<const RoutingSnapshot> snapshot) noexcept {
    if (!snapshot) {
        return false;
    }
    auto observed = acquire();
    while (snapshot->version() > observed->version()) {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
        const bool exchanged = current_.compare_exchange_weak(
            observed, snapshot, std::memory_order_acq_rel, std::memory_order_acquire);
#else
        const bool exchanged = std::atomic_compare_exchange_weak_explicit(
            &current_, &observed, snapshot, std::memory_order_acq_rel, std::memory_order_acquire);
#endif
        if (exchanged) {
            return true;
        }
    }
    return false;
}

} // namespace forwarding
