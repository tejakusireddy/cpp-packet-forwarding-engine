#include "forwarding/next_hop.hpp"

#include <unordered_set>
#include <utility>

namespace forwarding {
namespace {
// Fixed integer mixing keeps selection independent of standard-library hashes.
std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}
} // namespace

bool NextHopTable::upsert(std::uint32_t group_id, std::span<const NextHop> members) {
    if (members.empty()) {
        return false;
    }
    std::unordered_set<std::uint32_t> ids;
    Group replacement;
    replacement.active.reserve(members.size());
    for (const auto& member : members) {
        if (!ids.insert(member.id).second) {
            return false;
        }
        if (member.available) {
            replacement.active.push_back(member);
        }
    }
    groups_.insert_or_assign(group_id, std::move(replacement));
    return true;
}

bool NextHopTable::erase(std::uint32_t group_id) {
    return groups_.erase(group_id) != 0;
}

ResolutionResult NextHopTable::resolve(const Fib& fib, IpVersion family,
                                        const IpAddress& destination,
                                        std::uint64_t flow_hash) const noexcept {
    const auto route = fib.lookup(family, destination);
    if (!route) {
        return ResolutionError::no_route;
    }
    const auto group = groups_.find(route->next_hop_group);
    if (group == groups_.end()) {
        return ResolutionError::missing_group;
    }
    const auto& active = group->second.active;
    if (active.empty()) {
        return ResolutionError::no_available_members;
    }
    const auto index = static_cast<std::size_t>(mix(flow_hash) % active.size());
    return ResolvedRoute{*route, active[index]};
}

} // namespace forwarding
