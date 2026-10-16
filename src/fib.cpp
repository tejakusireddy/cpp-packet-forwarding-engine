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

Fib::Node* Fib::writable(std::shared_ptr<Node>& node) {
    if (!node) {
        node = std::make_shared<Node>();
    } else if (node.use_count() > 1) {
        node = std::make_shared<Node>(*node);
    }
    return node.get();
}

bool Fib::insert(IpVersion family, const IpAddress& prefix, std::size_t prefix_length,
                 std::uint32_t next_hop_group) {
    const auto bits = address_bits(family);
    if (bits == 0 || prefix_length > bits) {
        return false;
    }
    auto* node = writable(roots_[family == IpVersion::v4 ? 0 : 1]);
    for (std::size_t bit = 0; bit < prefix_length; ++bit) {
        auto& child = node->children[bit_at(prefix, bit)];
        node = writable(child);
    }
    if (!node->next_hop_group) {
        ++size_;
    }
    node->next_hop_group = next_hop_group;
    return true;
}

bool Fib::erase(IpVersion family, const IpAddress& prefix,
                std::size_t prefix_length) {
    const auto bits = address_bits(family);
    if (bits == 0 || prefix_length > bits) {
        return false;
    }
    // Check existence before allocating copies for a deletion that cannot match.
    const auto* existing = roots_[family == IpVersion::v4 ? 0 : 1].get();
    for (std::size_t bit = 0; existing && bit < prefix_length; ++bit) {
        existing = existing->children[bit_at(prefix, bit)].get();
    }
    if (!existing || !existing->next_hop_group) {
        return false;
    }
    auto* node = writable(roots_[family == IpVersion::v4 ? 0 : 1]);
    std::array<std::shared_ptr<Node>*, 128> path{};
    for (std::size_t bit = 0; bit < prefix_length; ++bit) {
        auto& child = node->children[bit_at(prefix, bit)];
        if (!child) {
            return false;
        }
        path[bit] = &child;
        node = writable(child);
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
    const auto* node = roots_[family == IpVersion::v4 ? 0 : 1].get();
    std::optional<FibMatch> best;
    for (std::size_t depth = 0; node; ++depth) {
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
