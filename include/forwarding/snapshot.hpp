#pragma once

#include "forwarding/next_hop.hpp"

#include <atomic>
#include <memory>

namespace forwarding {

struct RouteEntry {
    IpVersion family;
    IpAddress prefix;
    std::size_t prefix_length;
    std::uint32_t next_hop_group;
};

struct NextHopGroup {
    std::uint32_t id;
    std::vector<NextHop> members;
};

struct RouteUpdate {
    RouteEntry route;
    bool remove = false;
};

// Owns both tables without exposing mutation. Construction copies supplied data;
// invalid prefixes, missing groups, or invalid/duplicate groups throw
// std::invalid_argument. Repeated route prefixes use the last supplied entry.
class RoutingSnapshot {
public:
    RoutingSnapshot(std::uint64_t version, std::span<const RouteEntry> routes,
                    std::span<const NextHopGroup> groups);
    // Incremental version: shares unchanged FIB nodes, copies the group table,
    // upserts groups, then applies route changes in order. Missing deletes are
    // idempotent. Group deletion requires a complete replacement snapshot.
    RoutingSnapshot(std::uint64_t version, const RoutingSnapshot& base,
                    std::span<const RouteUpdate> updates, std::span<const NextHopGroup> groups);
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }
    [[nodiscard]] const Fib& fib() const noexcept { return fib_; }
    [[nodiscard]] const NextHopTable& next_hops() const noexcept { return next_hops_; }

private:
    const std::uint64_t version_;
    Fib fib_;
    NextHopTable next_hops_;
};

// Starts with an empty version-zero snapshot. Publication is atomic and strictly
// increases versions, including with concurrent publishers. shared_ptr atomics
// are not guaranteed lock-free; this is a correctness baseline for publication.
class SnapshotStore {
public:
    SnapshotStore();
    SnapshotStore(const SnapshotStore&) = delete;
    SnapshotStore& operator=(const SnapshotStore&) = delete;
    SnapshotStore(SnapshotStore&&) = delete;
    SnapshotStore& operator=(SnapshotStore&&) = delete;
    [[nodiscard]] std::shared_ptr<const RoutingSnapshot> acquire() const noexcept;
    // Null, equal, and older versions are rejected, leaving the store unchanged.
    [[nodiscard]] bool publish(std::shared_ptr<const RoutingSnapshot> snapshot) noexcept;
    // Publish only if the current version still equals the batch's base.
    [[nodiscard]] bool publish_if_base(std::shared_ptr<const RoutingSnapshot> snapshot,
                                       std::uint64_t base_version) noexcept;

private:
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<const RoutingSnapshot>> current_;
#else
    // Compatibility for libraries without the C++20 specialization.
    std::shared_ptr<const RoutingSnapshot> current_;
#endif
};

} // namespace forwarding
