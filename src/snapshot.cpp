#include "forwarding/snapshot.hpp"

#include <stdexcept>
#include <algorithm>
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

RoutingSnapshot::RoutingSnapshot(std::uint64_t version, const RoutingSnapshot& base,
                                 std::span<const RouteUpdate> updates,
                                 std::span<const NextHopGroup> groups)
    : version_(version), fib_(base.fib_), next_hops_(base.next_hops_) {
    if (version <= base.version()) {
        throw std::invalid_argument("Incremental version must exceed base version");
    }
    std::unordered_set<std::uint32_t> ids;
    for (const auto& group : groups) {
        if (!ids.insert(group.id).second || !next_hops_.upsert(group.id, group.members)) {
            throw std::invalid_argument("Invalid or duplicate next-hop group");
        }
    }
    for (const auto& update : updates) {
        const auto& route = update.route;
        const auto bits = route.family == IpVersion::v4 ? 32U :
                          route.family == IpVersion::v6 ? 128U : 0U;
        if (bits == 0 || route.prefix_length > bits) {
            throw std::invalid_argument("Invalid route prefix");
        }
        if (update.remove) {
            (void)fib_.erase(route.family, route.prefix, route.prefix_length);
        } else if (!next_hops_.contains(route.next_hop_group)) {
            throw std::invalid_argument("Route references a missing next-hop group");
        } else {
            (void)fib_.insert(route.family, route.prefix, route.prefix_length, route.next_hop_group);
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

void SnapshotStore::collect_locked() {
    std::erase_if(retired_, [](const auto& snapshot) { return snapshot.use_count() == 1; });
}

std::size_t SnapshotStore::collect_retired() {
    const std::lock_guard lock(publishers_);
    const auto previous = retired_.size();
    collect_locked();
    return previous - retired_.size();
}

bool SnapshotStore::publish(std::shared_ptr<const RoutingSnapshot> snapshot) {
    if (!snapshot) {
        return false;
    }
    const std::lock_guard lock(publishers_);
    collect_locked();
    auto observed = acquire();
    if (snapshot->version() <= observed->version()) return false;
    // Keep the old version alive before replacing current, even if a worker
    // releases its reference immediately after the atomic exchange.
    retired_.push_back(observed);
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

bool SnapshotStore::publish_if_base(std::shared_ptr<const RoutingSnapshot> snapshot,
                                   std::uint64_t base_version) {
    if (!snapshot || snapshot->version() <= base_version) return false;
    const std::lock_guard lock(publishers_);
    collect_locked();
    auto observed = acquire();
    if (observed->version() != base_version) return false;
    retired_.push_back(observed);
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    return current_.compare_exchange_strong(observed, snapshot,
        std::memory_order_acq_rel, std::memory_order_acquire);
#else
    return std::atomic_compare_exchange_strong_explicit(&current_, &observed, snapshot,
        std::memory_order_acq_rel, std::memory_order_acquire);
#endif
}

} // namespace forwarding
