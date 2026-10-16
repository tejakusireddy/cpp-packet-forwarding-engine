#pragma once

#include "forwarding/control.hpp"

#include <string>
#include <string_view>

namespace forwarding {

struct RoutingRequest {
    std::string socket_path;
    UpdateBatch batch;
};

// Arguments exclude the executable name. Strict parsing preserves 64-bit version
// values and rejects malformed CIDR/MAC/boolean fields before opening a socket.
[[nodiscard]] RoutingRequest parse_routing_request(std::span<const std::string_view> arguments);

} // namespace forwarding
