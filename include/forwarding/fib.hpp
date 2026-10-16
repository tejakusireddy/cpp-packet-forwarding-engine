#pragma once

#include "forwarding/packet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace forwarding {

using IpAddress = std::array<std::uint8_t, 16>;

struct FibMatch {
    std::uint32_t next_hop_group;
    std::size_t prefix_length;
    bool operator==(const FibMatch&) const = default;
};

// Mutable binary prefix trie. Concurrent readers are supported only while no
// thread mutates the table. Publication of immutable snapshots belongs to IPC.
class Fib {
public:
    Fib() = default;
    Fib(const Fib&) = delete;
    Fib& operator=(const Fib&) = delete;
    Fib(Fib&&) = delete;
    Fib& operator=(Fib&&) = delete;

    // Host bits are ignored. IPv4 uses the first four bytes of the address.
    // Returns false for an invalid family or prefix length; otherwise upserts.
    [[nodiscard]] bool insert(IpVersion family, const IpAddress& prefix,
                              std::size_t prefix_length, std::uint32_t next_hop_group);
    // Removes the exact prefix, leaving more-specific routes intact.
    [[nodiscard]] bool erase(IpVersion family, const IpAddress& prefix,
                             std::size_t prefix_length) noexcept;
    [[nodiscard]] std::optional<FibMatch> lookup(IpVersion family,
                                                const IpAddress& address) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    struct Node {
        std::array<std::unique_ptr<Node>, 2> children;
        std::optional<std::uint32_t> next_hop_group;
    };
    std::array<Node, 2> roots_;
    std::size_t size_ = 0;
};

} // namespace forwarding
