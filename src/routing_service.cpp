#include "forwarding/routing_service.hpp"

#include <cerrno>
#include <system_error>

namespace forwarding {
using namespace std::chrono_literals;

RoutingService::RoutingService(SnapshotStore& store, std::string path,
                               std::optional<std::uint32_t> output_interface,
                               std::chrono::milliseconds timeout)
    : store_(store), listener_(std::move(path)), control_(store),
      output_interface_(output_interface), timeout_(timeout) {
    if (timeout.count() <= 0 || timeout > std::chrono::hours(1))
        throw std::invalid_argument("Routing service timeout must be in (0, 1 hour]");
    control_thread_ = std::thread([this] {
        while (!control_.finished()) {
            work_.acquire();
            (void)control_.poll();
        }
    });
    try {
        transport_thread_ = std::jthread([this](std::stop_token stop) { serve(stop); });
    } catch (...) {
        control_.close();
        work_.release();
        control_thread_.join();
        throw;
    }
}

RoutingService::~RoutingService() { stop(); }
void RoutingService::stop() noexcept {
    transport_thread_.request_stop();
    if (transport_thread_.joinable()) transport_thread_.join();
    if (control_thread_.joinable()) control_thread_.join();
}

void RoutingService::serve(std::stop_token stop) noexcept {
    while (!stop.stop_requested()) {
        try {
            auto client = listener_.try_accept();
            if (!client) {
                // Collect on an adapter/control thread, never on a packet worker.
                (void)store_.collect_retired();
                std::this_thread::sleep_for(1ms);
                continue;
            }
            for (unsigned message = 0; message < 64 && !stop.stop_requested(); ++message) {
                const ipc::IoOptions options{timeout_, stop};
                auto batch = ipc::receive_batch(client->get(), options);
                bool allowed = true;
                for (const auto& group : batch.groups) {
                    for (const auto& member : group.members) {
                        if (output_interface_ && member.interface_id != *output_interface_) allowed = false;
                    }
                }
                UpdateAck ack{batch.version, store_.acquire()->version(), UpdateStatus::invalid};
                if (allowed) {
                    while (!control_.try_submit(std::move(batch))) {
                        if (stop.stop_requested()) throw std::system_error(ECANCELED, std::generic_category());
                        std::this_thread::yield();
                    }
                    // Exactly one batch is outstanding. Wake the control thread
                    // once for it; idle routing must not consume a forwarding CPU.
                    work_.release();
                    // Once submitted, always consume its ack, even during stop.
                    // Otherwise shutdown could strand a backpressured control thread.
                    std::optional<UpdateAck> completed;
                    while (!(completed = control_.try_receive())) std::this_thread::yield();
                    ack = *completed;
                }
                ipc::send_ack(client->get(), ack, options);
            }
        } catch (const std::system_error&) {
            // Disconnect, deadline, or cancellation closes this RAII connection.
            // A new client can retry with an explicit base/version; never replay.
            std::this_thread::sleep_for(1ms);
        } catch (const std::invalid_argument&) {
            // Malformed framing closes the stream without publishing its contents.
        } catch (const std::bad_alloc&) {
            // Bounded messages can still encounter memory pressure. Close and
            // back off; an already accepted batch retains its control-plane result.
            std::this_thread::sleep_for(1ms);
        }
    }
    control_.close();
    work_.release();
}

} // namespace forwarding
