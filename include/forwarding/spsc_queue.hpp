#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace forwarding {

// Exactly one producer calls try_push and one consumer calls try_pop. Construct
// before starting either thread; destroy only after both threads have stopped.
// Capacity is the usable element count. One extra slot distinguishes full/empty.
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity > 0 && Capacity < std::numeric_limits<std::size_t>::max());
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "SpscQueue requires lock-free size_t atomics");
    static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>,
                  "Queue elements must support non-throwing move construction and destruction");

public:
    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    // Returns false if full without moving from value. Operations never wait.
    [[nodiscard]] bool try_push(T&& value) noexcept { return push(std::move(value)); }

    [[nodiscard]] bool try_push(const T& value) noexcept
        requires std::is_nothrow_copy_constructible_v<T> {
        return push(value);
    }

    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const auto tail = consumer_.position.load(std::memory_order_relaxed);
        if (tail == producer_.position.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        std::optional<T> result(std::in_place, std::move(*slots_[tail]));
        slots_[tail].reset();
        // Publish slot reuse only after the consumer has finished accessing it.
        consumer_.position.store(next(tail), std::memory_order_release);
        return result;
    }

private:
    struct alignas(64) Cursor {
        std::atomic<std::size_t> position{0};
    };

    static std::size_t next(std::size_t position) noexcept {
        return position == Capacity ? 0 : position + 1;
    }

    template <typename U>
    bool push(U&& value) noexcept {
        const auto head = producer_.position.load(std::memory_order_relaxed);
        const auto following = next(head);
        if (following == consumer_.position.load(std::memory_order_acquire)) {
            return false;
        }
        slots_[head].emplace(std::forward<U>(value));
        // Make the fully constructed element visible before publishing its slot.
        producer_.position.store(following, std::memory_order_release);
        return true;
    }

    Cursor producer_;
    Cursor consumer_;
    alignas(64) std::array<std::optional<T>, Capacity + 1> slots_{};
};

} // namespace forwarding
