#pragma once

#include "forwarding/unix_socket.hpp"

namespace forwarding {

// Optional process-facing adapter. One transport thread owns all queue submission
// and acknowledgment consumption; a separate control thread builds snapshots.
// One active client, backlog four, at most 64 messages per connection. No socket
// work occurs on forwarding threads. Store must outlive the service.
class RoutingService {
public:
    RoutingService(SnapshotStore& store, std::string path,
                   std::optional<std::uint32_t> output_interface = {},
                   std::chrono::milliseconds timeout = std::chrono::seconds(1));
    ~RoutingService();
    RoutingService(const RoutingService&) = delete;
    RoutingService& operator=(const RoutingService&) = delete;
    // Cancels socket waits, drains any accepted control batch, and joins both
    // threads. Safe to call repeatedly from the lifecycle-owning thread.
    void stop() noexcept;
private:
    void serve(std::stop_token stop) noexcept;
    SnapshotStore& store_;
    ipc::UnixListener listener_;
    ControlPlane<8> control_;
    std::optional<std::uint32_t> output_interface_;
    std::chrono::milliseconds timeout_;
    std::thread control_thread_;
    std::jthread transport_thread_;
};

} // namespace forwarding
