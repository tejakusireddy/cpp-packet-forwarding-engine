#include "forwarding/fib.hpp"

namespace forwarding {
namespace {
std::size_t address_bits(IpVersion family) noexcept {
    switch (family) {
    case IpVersion::v4: return 32;
    case IpVersion::v6: return 128;
    }
    return 0;
}

std::size_t bit_at(const IpAddress& address, std::size_t bit) noexcept {
    return (static_cast<unsigned>(address[bit / 8]) >> (7 - bit % 8)) & 1U;
}
} // namespace

bool Fib::insert(IpVersion family, const IpAddress& prefix, std::size_t prefix_length,
                 std::uint32_t next_hop_group) {
    const auto bits = address_bits(family);
    if (bits == 0 || prefix_length > bits) {
        return false;
    }
    auto* node = &roots_[family == IpVersion::v4 ? 0 : 1];
    for (std::size_t bit = 0; bit < prefix_length; ++bit) {
        auto& child = node->children[bit_at(prefix, bit)];
        if (!child) {
            child = std::make_unique<Node>();
        }
        node = child.get();
    }
    if (!node->next_hop_group) {
        ++size_;
    }
    node->next_hop_group = next_hop_group;
    return true;
}

bool Fib::erase(IpVersion family, const IpAddress& prefix,
                std::size_t prefix_length) noexcept {
    const auto bits = address_bits(family);
    if (bits == 0 || prefix_length > bits) {
        return false;
    }
    auto* node = &roots_[family == IpVersion::v4 ? 0 : 1];
    std::array<std::unique_ptr<Node>*, 128> path{};
    for (std::size_t bit = 0; bit < prefix_length; ++bit) {
        auto& child = node->children[bit_at(prefix, bit)];
        if (!child) {
            return false;
        }
        path[bit] = &child;
        node = child.get();
    }
    if (!node->next_hop_group) {
        return false;
    }
    node->next_hop_group.reset();
    --size_;
    // Release unused nodes from leaf to root without touching live descendants.
    for (auto depth = prefix_length; depth > 0; --depth) {
        auto& child = *path[depth - 1];
        if (child->next_hop_group || child->children[0] || child->children[1]) {
            break;
        }
        child.reset();
    }
    return true;
}

std::optional<FibMatch> Fib::lookup(IpVersion family, const IpAddress& address) const noexcept {
    const auto bits = address_bits(family);
    if (bits == 0) {
        return std::nullopt;
    }
    const auto* node = &roots_[family == IpVersion::v4 ? 0 : 1];
    std::optional<FibMatch> best;
    for (std::size_t depth = 0;; ++depth) {
        if (node->next_hop_group) {
            best = FibMatch{*node->next_hop_group, depth};
        }
        if (depth == bits) {
            break;
        }
        node = node->children[bit_at(address, depth)].get();
        if (!node) {
            break;
        }
    }
    return best;
}

} // namespace forwarding
