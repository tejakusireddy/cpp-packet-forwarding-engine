#pragma once

#include "forwarding/snapshot.hpp"
#include "forwarding/spsc_queue.hpp"

#include <new>
#include <stdexcept>
#include <thread>

namespace forwarding {

struct UpdateBatch {
    std::uint64_t base_version;
    std::uint64_t version;
    std::vector<RouteUpdate> routes;
    std::vector<NextHopGroup> groups;
};

enum class UpdateStatus { applied, stale_base, invalid, resource_exhausted };
struct UpdateAck {
    std::uint64_t version;
    std::uint64_t current_version;
    UpdateStatus status;
};

// One submitter, one control processing thread, one acknowledgment consumer.
// Owns no thread; close/drain/join follows the same contract as forwarding workers.
// Snapshot construction and update allocation happen on the control thread.
template <std::size_t Capacity>
class ControlPlane {
public:
    explicit ControlPlane(SnapshotStore& store) noexcept : store_(store) {}

    [[nodiscard]] bool try_submit(UpdateBatch&& batch) noexcept {
        return !closed_.load(std::memory_order_relaxed) && input_.try_push(std::move(batch));
    }
    void close() noexcept { closed_.store(true, std::memory_order_release); }
    [[nodiscard]] std::optional<UpdateAck> try_receive() noexcept { return output_.try_pop(); }
    [[nodiscard]] bool finished() const noexcept { return finished_.load(std::memory_order_acquire); }

    // False means idle/backpressured/stopped. A pending acknowledgment prevents
    // the next update from being consumed, so publication is never repeated.
    [[nodiscard]] bool poll() noexcept {
        if (finished()) return false;
        if (!pending_) {
            const bool closed = closed_.load(std::memory_order_acquire);
            auto batch = input_.try_pop();
            if (!batch) {
                if (closed) finished_.store(true, std::memory_order_release);
                return false;
            }
            UpdateStatus status = UpdateStatus::stale_base;
            const auto base = store_.acquire();
            if (base->version() == batch->base_version) {
                try {
                    auto next = std::make_shared<const RoutingSnapshot>(
                        batch->version, *base, batch->routes, batch->groups);
                    status = store_.publish_if_base(std::move(next), batch->base_version)
                                 ? UpdateStatus::applied : UpdateStatus::stale_base;
                } catch (const std::invalid_argument&) {
                    status = UpdateStatus::invalid;
                } catch (const std::bad_alloc&) {
                    status = UpdateStatus::resource_exhausted;
                }
            }
            pending_ = UpdateAck{batch->version, store_.acquire()->version(), status};
        }
        if (!output_.try_push(std::move(*pending_))) return false;
        pending_.reset();
        return true;
    }
    void run() noexcept {
        while (!finished()) {
            if (!poll()) std::this_thread::yield();
        }
    }

private:
    SnapshotStore& store_;
    SpscQueue<UpdateBatch, Capacity> input_;
    SpscQueue<UpdateAck, Capacity> output_;
    std::optional<UpdateAck> pending_;
    std::atomic<bool> closed_{false};
    std::atomic<bool> finished_{false};
};

} // namespace forwarding
