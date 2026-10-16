#include "forwarding/spsc_queue.hpp"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

using forwarding::SpscQueue;

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <std::size_t Capacity>
void boundaries() {
    SpscQueue<unsigned, Capacity> queue;
    check(queue.capacity() == Capacity, "usable capacity");
    check(!queue.try_pop(), "initially empty");
    for (unsigned cycle = 0; cycle < 1000; ++cycle) {
        for (unsigned i = 0; i < Capacity; ++i) {
            check(queue.try_push(i), "fill to exact capacity");
        }
        check(!queue.try_push(999), "full queue accepted element");
        for (unsigned i = 0; i < Capacity; ++i) {
            const auto value = queue.try_pop();
            check(value && *value == i, "FIFO across wraparound");
        }
        check(!queue.try_pop(), "drained queue not empty");
    }
}

struct Tracked {
    static inline unsigned alive = 0;
    std::unique_ptr<int> value;
    explicit Tracked(int input) : value(std::make_unique<int>(input)) { ++alive; }
    Tracked(Tracked&& other) noexcept : value(std::move(other.value)) { ++alive; }
    ~Tracked() { --alive; }
};

void ownership() {
    {
        SpscQueue<Tracked, 1> queue;
        Tracked first(7);
        Tracked second(8);
        check(queue.try_push(std::move(first)), "push move-only element");
        check(!first.value, "successful push did not move ownership");
        check(!queue.try_push(std::move(second)) && second.value && *second.value == 8,
              "full push consumed ownership");
        {
            auto value = queue.try_pop();
            check(value && *value->value == 7, "pop move-only element");
        }
        check(queue.try_push(std::move(second)), "reuse slot after move-only pop");
        // The queued element is intentionally left for queue destruction.
    }
    check(Tracked::alive == 0, "element lifetime leak");
}

struct Message {
    std::uint64_t sequence;
    std::uint64_t complement;
    std::array<std::uint64_t, 4> payload;
};

template <std::size_t Capacity>
void concurrent() {
    SpscQueue<Message, Capacity> queue;
    constexpr std::uint64_t count = 250000;
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < count; ++i) {
            Message message{i, ~i, {i + 1, i + 2, i + 3, i + 4}};
            while (!queue.try_push(std::move(message))) {
                std::this_thread::yield();
            }
        }
    });
    std::thread consumer([&] {
        for (std::uint64_t i = 0; i < count; ++i) {
            std::optional<Message> message;
            while (!(message = queue.try_pop())) {
                std::this_thread::yield();
            }
            check(message->sequence == i && message->complement == ~i, "concurrent FIFO/integrity");
            for (std::size_t index = 0; index < message->payload.size(); ++index) {
                check(message->payload[index] == i + index + 1, "incomplete payload publication");
            }
        }
    });
    producer.join();
    consumer.join();
    check(!queue.try_pop(), "concurrent queue not drained");
}
} // namespace

int main() {
    boundaries<1>();
    boundaries<3>();
    boundaries<16>();
    ownership();
    concurrent<1>();
    concurrent<3>();
    concurrent<1024>();
    std::cout << "All SPSC queue tests passed\n";
}
