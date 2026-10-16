#include "forwarding/flow.hpp"
#include "forwarding/linux_io.hpp"
#include "forwarding/packet_pool.hpp"
#include "forwarding/worker.hpp"

#include <charconv>
#include <algorithm>
#include <csignal>
#include <iostream>
#include <limits>
#include <string>

using namespace forwarding;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
std::size_t number(const char* text) {
    const std::string value(text);
    std::size_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::invalid_argument("Invalid numeric argument");
    return result;
}
MacAddress mac(const char* text) {
    const std::string value(text);
    if (value.size() != 17) throw std::invalid_argument("Expected aa:bb:cc:dd:ee:ff MAC address");
    MacAddress result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        unsigned byte = 0;
        const auto begin = value.data() + i * 3;
        const auto parsed = std::from_chars(begin, begin + 2, byte, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != begin + 2 || byte > 255 ||
            (i < 5 && value[i * 3 + 2] != ':')) throw std::invalid_argument("Invalid MAC address");
        result[i] = static_cast<std::uint8_t>(byte);
    }
    return result;
}
struct Workers {
    std::vector<std::unique_ptr<ForwardingWorker<64>>> workers;
    std::vector<std::thread> threads;
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> pin_failed{false};
    Workers(std::size_t count, SnapshotStore& store, std::optional<unsigned> cpu) {
        for (std::size_t i = 0; i < count; ++i)
            workers.push_back(std::make_unique<ForwardingWorker<64>>(store));
        try {
            for (std::size_t i = 0; i < count; ++i) {
                threads.emplace_back([&, i, cpu] {
                    if (cpu) {
                        try { pin_current_thread(*cpu + static_cast<unsigned>(i)); }
                        catch (...) { pin_failed.store(true, std::memory_order_relaxed); }
                    }
                    ready.fetch_add(1, std::memory_order_release);
                    workers[i]->run();
                });
            }
            while (ready.load(std::memory_order_acquire) != count) std::this_thread::yield();
            if (pin_failed.load(std::memory_order_relaxed)) throw std::runtime_error("Worker CPU affinity failed");
        } catch (...) { drain(); throw; }
    }
    void drain() noexcept {
        for (std::size_t i = 0; i < threads.size(); ++i) workers[i]->close();
        for (std::size_t i = 0; i < threads.size(); ++i) {
            while (!workers[i]->finished()) {
                (void)workers[i]->try_receive();
                std::this_thread::yield();
            }
            if (threads[i].joinable()) threads[i].join();
        }
    }
    ~Workers() { drain(); }
};
}

int main(int argc, char** argv) {
    if (argc < 4 || argc > 7) {
        std::cerr << "Usage: packet_forwarder IN OUT DEST_MAC [WORKERS=1] [PACKETS=0] [FIRST_CPU]\n";
        return 2;
    }
    try {
        const auto destination_mac = mac(argv[3]);
        const auto count = argc > 4 ? number(argv[4]) : 1;
        const auto limit = argc > 5 ? number(argv[5]) : 0;
        if (count == 0 || count > 64) throw std::invalid_argument("Workers must be in 1..64");
        std::optional<unsigned> cpu;
        if (argc > 6) {
            const auto first = number(argv[6]);
            if (first > std::numeric_limits<unsigned>::max() - count)
                throw std::invalid_argument("CPU range overflow");
            cpu = static_cast<unsigned>(first);
        }
        PacketSocket input(argv[1]), output(argv[2]);
        if (input.interface_id() == output.interface_id()) throw std::invalid_argument("Ingress and egress must differ");
        const NextHop hop{0, output.interface_id(), output.mac_address(), destination_mac};
        const std::array groups{NextHopGroup{0, {hop}}};
        const std::array routes{RouteEntry{IpVersion::v4, {}, 0, 0}, RouteEntry{IpVersion::v6, {}, 0, 0}};
        SnapshotStore store;
        if (!store.publish(std::make_shared<const RoutingSnapshot>(1, routes, groups)))
            throw std::runtime_error("Initial routing publication failed");
        Workers processing(count, store, cpu);
        const auto frame_capacity = std::max<std::size_t>(60, input.mtu() + 14);
        PacketPool pool(count * 130 + 1, frame_capacity);
        std::optional<PacketJob> pending;
        std::size_t target = 0;
        std::vector<std::optional<PacketCompletion>> transmit(count);
        std::size_t received = 0, accepted = 0, completed = 0, forwarded = 0, dropped = 0;
        bool closed = false;
        std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
        std::cout << "ready\n" << std::flush;
        for (;;) {
            if (!closed && !pending && !interrupted && (limit == 0 || received < limit)) {
                auto buffer = pool.acquire(frame_capacity);
                if (buffer) {
                    std::optional<std::size_t> size;
                    try { size = input.receive(*buffer); }
                    catch (const std::length_error&) { ++dropped; }
                    if (size) {
                        buffer->resize(*size);
                        const auto parsed = parse_packet(*buffer);
                        target = std::holds_alternative<PacketView>(parsed)
                                     ? static_cast<std::size_t>(flow_hash(std::get<PacketView>(parsed)) % count) : 0;
                        pending.emplace(PacketJob{received++, std::move(*buffer)});
                    } else if (!pool.recycle(std::move(*buffer))) std::terminate();
                }
            }
            if (pending && processing.workers[target]->try_submit(std::move(*pending))) {
                pending.reset(); ++accepted;
            }
            if (!closed && !pending && (interrupted || (limit != 0 && received >= limit))) {
                for (auto& worker : processing.workers) worker->close();
                closed = true;
            }
            for (std::size_t i = 0; i < count; ++i) {
                if (!transmit[i]) transmit[i] = processing.workers[i]->try_receive();
                if (!transmit[i]) continue;
                const auto* result = std::get_if<ForwardedPacket>(&transmit[i]->result);
                bool sent = false;
                if (result) {
                    try { sent = output.transmit(std::span(transmit[i]->frame).first(result->frame_length)); }
                    catch (const std::length_error&) { result = nullptr; }
                    if (result && !sent) continue;
                }
                if (sent) ++forwarded; else ++dropped;
                if (!pool.recycle(std::move(transmit[i]->frame))) std::terminate();
                transmit[i].reset(); ++completed;
            }
            if (closed && completed == accepted) break;
            std::this_thread::yield();
        }
        processing.drain();
        (void)store.collect_retired();
        std::cout << "received=" << received << " forwarded=" << forwarded << " dropped=" << dropped << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
