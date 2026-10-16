#include "forwarding/worker.hpp"

#include <barrier>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace forwarding;

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::shared_ptr<const RoutingSnapshot> snapshot(std::uint64_t version) {
    const auto marker = static_cast<std::uint32_t>(version);
    const std::array groups{NextHopGroup{marker, {
        NextHop{marker, marker, {2, 0, 0, 0, 0, 1}, {2, 0, 0, 0, 0, 2}}}}};
    const std::array routes{RouteEntry{IpVersion::v4, {}, 0, marker},
                            RouteEntry{IpVersion::v6, {}, 0, marker}};
    return std::make_shared<const RoutingSnapshot>(version, routes, groups);
}

void verify(const std::shared_ptr<const RoutingSnapshot>& current) {
    for (const auto family : {IpVersion::v4, IpVersion::v6}) {
        const auto result = current->next_hops().resolve(current->fib(), family, {}, 42);
        if (current->version() == 0) {
            check(result == ResolutionResult{ResolutionError::no_route}, "initial empty snapshot");
        } else {
            check(std::holds_alternative<ResolvedRoute>(result), "inconsistent snapshot tables");
            const auto marker = static_cast<std::uint32_t>(current->version());
            const auto& route = std::get<ResolvedRoute>(result);
            check(route.route.next_hop_group == marker && route.next_hop.id == marker &&
                  route.next_hop.interface_id == marker, "mixed snapshot versions");
        }
    }
}

template <typename Function>
void invalid(Function build) {
    bool threw = false;
    try {
        build();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "invalid snapshot was accepted");
}

void validation_and_lifetime() {
    SnapshotStore store;
    verify(store.acquire());
    check(!store.publish(nullptr) && !store.publish(snapshot(0)), "reject null/zero publication");
    check(store.publish(snapshot(10)), "publish first snapshot");
    auto held = store.acquire();
    const std::weak_ptr<const RoutingSnapshot> retired = held;
    check(!store.publish(snapshot(10)) && !store.publish(snapshot(9)), "reject stale versions");
    check(store.publish(snapshot(11)), "publish newer snapshot");
    verify(held);
    check(held->version() == 10 && !retired.expired(), "old reader lost snapshot");
    held.reset();
    check(retired.expired(), "unused retired snapshot not reclaimed");

    const auto hop = NextHop{1, 1, {}, {}};
    std::array groups{NextHopGroup{1, {hop}}};
    std::array routes{RouteEntry{IpVersion::v4, {}, 24, 1}};
    const auto copied = std::make_shared<const RoutingSnapshot>(12, routes, groups);
    groups[0].members[0].id = 99;
    routes[0].next_hop_group = 99;
    const auto resolution = copied->next_hops().resolve(copied->fib(), IpVersion::v4, {}, 0);
    check(std::get<ResolvedRoute>(resolution).next_hop.id == 1, "snapshot aliases input buffers");
    invalid([&] { RoutingSnapshot bad(12, routes, groups); });
    routes[0].next_hop_group = 1;
    routes[0].prefix_length = 33;
    invalid([&] { RoutingSnapshot bad(12, routes, groups); });
    routes[0].prefix_length = 24;
    groups[0].members.clear();
    invalid([&] { RoutingSnapshot bad(12, routes, groups); });
    groups[0].members = {hop, hop};
    invalid([&] { RoutingSnapshot bad(12, routes, groups); });
    groups[0].members = {hop};
    const std::array duplicate_groups{groups[0], groups[0]};
    invalid([&] { RoutingSnapshot bad(12, routes, duplicate_groups); });
    check(store.acquire()->version() == 11, "failed build changed published state");

    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    check(store.publish(snapshot(maximum)), "publish maximum version");
    check(!store.publish(snapshot(0)) && !store.publish(snapshot(maximum)), "version wraparound");
}

void concurrent_publication() {
    SnapshotStore store;
    std::barrier start(6);
    std::atomic<unsigned> done{0};
    std::array<std::thread, 2> publishers;
    std::array<std::thread, 4> readers;
    for (std::size_t writer = 0; writer < publishers.size(); ++writer) {
        publishers[writer] = std::thread([&, writer] {
            start.arrive_and_wait();
            for (std::uint64_t version = writer + 1; version <= 400; version += 2) {
                const bool published = store.publish(snapshot(version));
                if (!published) {
                    check(store.acquire()->version() >= version, "incorrect version rejection");
                }
            }
            done.fetch_add(1, std::memory_order_release);
        });
    }
    for (auto& reader : readers) {
        reader = std::thread([&] {
            start.arrive_and_wait();
            std::uint64_t last = 0;
            std::size_t reads = 0;
            while (done.load(std::memory_order_acquire) != 2 || reads < 2000) {
                const auto current = store.acquire();
                check(current->version() >= last, "reader version regression");
                last = current->version();
                verify(current);
                ++reads;
            }
        });
    }
    for (auto& publisher : publishers) publisher.join();
    for (auto& reader : readers) reader.join();
    check(store.acquire()->version() == 400, "latest version missing after publisher race");
}

PacketJob packet(std::uint64_t id) {
    std::vector<std::uint8_t> frame(58, 0);
    frame[12] = 0x86;
    frame[13] = 0xdd;
    frame[14] = 0x60;
    frame[19] = 4;
    frame[20] = 17;
    frame[21] = 64;
    return PacketJob{id, std::move(frame)};
}

void check_completion(const PacketCompletion& completion, std::uint64_t version) {
    check(completion.routing_version == version, "completion lost its processing version");
    const auto& forwarded = std::get<ForwardedPacket>(completion.result);
    check(forwarded.resolved.next_hop.id == version && completion.frame[21] == 63,
          "worker mixed versions or forwarded twice");
}

void worker_updates() {
    SnapshotStore store;
    check(store.publish(snapshot(1)), "initial worker snapshot");
    ForwardingWorker<1> worker(store);
    check(worker.try_submit(packet(1)) && worker.poll() == WorkerProgress::progressed,
          "first worker snapshot");
    check(store.publish(snapshot(2)), "second worker snapshot");
    check(worker.try_submit(packet(2)) && worker.poll() == WorkerProgress::backpressure,
          "hold completion across update");
    check(store.publish(snapshot(3)), "third worker snapshot");
    auto completion = worker.try_receive();
    check(completion.has_value(), "missing first completion");
    check_completion(*completion, 1);
    check(worker.poll() == WorkerProgress::progressed, "publish pending completion");
    completion = worker.try_receive();
    check(completion.has_value(), "missing second completion");
    check_completion(*completion, 2);
    check(worker.try_submit(packet(3)), "submit after publication");
    worker.close();
    worker.run();
    completion = worker.try_receive();
    check(completion.has_value(), "missing third completion");
    check_completion(*completion, 3);

    ForwardingWorker<8> live(store);
    std::barrier start(3);
    std::thread processing([&] { start.arrive_and_wait(); live.run(); });
    std::thread publishing([&] {
        start.arrive_and_wait();
        for (std::uint64_t version = 4; version <= 203; ++version) {
            check(store.publish(snapshot(version)), "publish during forwarding");
        }
    });
    start.arrive_and_wait();
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
    std::uint64_t last_version = 3;
    while (received < 4000) {
        if (sent < 4000 && live.try_submit(packet(sent))) {
            if (++sent == 4000) live.close();
        }
        if (const auto result = live.try_receive()) {
            check(result->id == received, "live update changed packet order");
            check(result->routing_version >= last_version, "worker version regression");
            last_version = result->routing_version;
            check_completion(*result, last_version);
            ++received;
        }
        std::this_thread::yield();
    }
    processing.join();
    publishing.join();
    check(live.finished() && store.acquire()->version() == 203, "live publication shutdown");
}
} // namespace

int main() {
    validation_and_lifetime();
    concurrent_publication();
    worker_updates();
    std::cout << "All routing snapshot tests passed\n";
}
