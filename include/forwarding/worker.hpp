#pragma once

#include "forwarding/forward.hpp"
#include "forwarding/spsc_queue.hpp"

#include <thread>
#include <vector>

namespace forwarding {

struct PacketJob {
    std::uint64_t id;
    std::vector<std::uint8_t> frame;
};

struct PacketCompletion {
    std::uint64_t id;
    std::vector<std::uint8_t> frame;
    ForwardResult result;
};

enum class WorkerProgress { progressed, idle, backpressure, stopped };

// One submitting thread, one processing thread, one completion-consuming thread.
// Tables must outlive the worker and remain unchanged until processing stops.
// The caller owns the processing thread and must join it before destruction.
template <std::size_t Capacity>
class ForwardingWorker {
public:
    ForwardingWorker(const Fib& fib, const NextHopTable& next_hops) noexcept
        : fib_(fib), next_hops_(next_hops) {}

    // On false (closed/full), ownership remains with the caller.
    [[nodiscard]] bool try_submit(PacketJob&& packet) noexcept {
        if (closed_.load(std::memory_order_relaxed)) {
            return false;
        }
        return input_.try_push(std::move(packet));
    }

    // Called only by the submitting thread, after its final submission.
    void close() noexcept { closed_.store(true, std::memory_order_release); }

    [[nodiscard]] std::optional<PacketCompletion> try_receive() noexcept {
        return output_.try_pop();
    }

    // Means all accepted jobs have been published as completions. The consumer
    // must still drain the output queue, including after this becomes true.
    [[nodiscard]] bool finished() const noexcept {
        return finished_.load(std::memory_order_acquire);
    }

    // Called only by the processing thread. Holds at most one pending completion
    // when output is full, without dropping packets or forwarding them twice.
    [[nodiscard]] WorkerProgress poll() noexcept {
        if (finished()) {
            return WorkerProgress::stopped;
        }
        if (!pending_) {
            // Acquire close before checking input so a final submission cannot
            // be missed when the producer closes concurrently with an empty poll.
            const bool closed = closed_.load(std::memory_order_acquire);
            auto packet = input_.try_pop();
            if (!packet) {
                if (closed) {
                    finished_.store(true, std::memory_order_release);
                    return WorkerProgress::stopped;
                }
                return WorkerProgress::idle;
            }
            auto result = forward_packet(packet->frame, fib_, next_hops_);
            pending_.emplace(PacketCompletion{packet->id, std::move(packet->frame), result});
        }
        if (!output_.try_push(std::move(*pending_))) {
            return WorkerProgress::backpressure;
        }
        pending_.reset();
        return WorkerProgress::progressed;
    }

    // Graceful drain. A consumer must keep receiving while this runs, including
    // after close, or a full completion queue will apply backpressure indefinitely.
    void run() noexcept {
        for (;;) {
            const auto progress = poll();
            if (progress == WorkerProgress::stopped) {
                return;
            }
            if (progress != WorkerProgress::progressed) {
                std::this_thread::yield();
            }
        }
    }

private:
    const Fib& fib_;
    const NextHopTable& next_hops_;
    SpscQueue<PacketJob, Capacity> input_;
    SpscQueue<PacketCompletion, Capacity> output_;
    std::optional<PacketCompletion> pending_;
    std::atomic<bool> closed_{false};
    std::atomic<bool> finished_{false};
};

} // namespace forwarding
