#include "forwarding/flow.hpp"
#include "forwarding/linux_io.hpp"
#include "forwarding/packet_pool.hpp"
#include "forwarding/worker.hpp"
#include "forwarding/routing_service.hpp"

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
std::size_t number(std::string_view value) {
    std::size_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::invalid_argument("Invalid numeric argument");
    return result;
}
MacAddress mac(std::string_view value) {
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
    if (argc < 4) {
        std::cerr << "Usage: packet_forwarder IN OUT DEST_MAC [WORKERS=1] [PACKETS=0] [FIRST_CPU]"
                     " [--control PATH] [--control-timeout-ms 1000]\n";
        return 2;
    }
    try {
        std::vector<std::string_view> arguments;
        std::optional<std::string> control_path;
        std::chrono::milliseconds control_timeout{1000};
        for (int i = 1; i < argc; ++i) {
            const std::string_view flag(argv[i]);
            if (flag == "--control") {
                if (control_path || i + 1 == argc) throw std::invalid_argument("Expected one control socket path");
                control_path = argv[++i];
            } else if (flag == "--control-timeout-ms") {
                if (i + 1 == argc) throw std::invalid_argument("Missing control timeout");
                const auto timeout = number(argv[++i]);
                if (timeout == 0 || timeout > 3600000) throw std::invalid_argument("Invalid control timeout");
                control_timeout = std::chrono::milliseconds(timeout);
            } else if (flag.starts_with("--")) {
                throw std::invalid_argument("Unknown runtime option");
            } else arguments.push_back(flag);
        }
        if (arguments.size() < 3 || arguments.size() > 6) throw std::invalid_argument("Incorrect positional arguments");
        const auto destination_mac = mac(arguments[2]);
        const auto count = arguments.size() > 3 ? number(arguments[3]) : 1;
        const auto limit = arguments.size() > 4 ? number(arguments[4]) : 0;
        if (count == 0 || count > 64) throw std::invalid_argument("Workers must be in 1..64");
        std::optional<unsigned> cpu;
        if (arguments.size() > 5) {
            const auto first = number(arguments[5]);
            if (first > std::numeric_limits<unsigned>::max() - count)
                throw std::invalid_argument("CPU range overflow");
            cpu = static_cast<unsigned>(first);
        }
        PacketSocket input(arguments[0]), output(arguments[1]);
        if (input.interface_id() == output.interface_id()) throw std::invalid_argument("Ingress and egress must differ");
        const NextHop hop{0, output.interface_id(), output.mac_address(), destination_mac};
        const std::array groups{NextHopGroup{0, {hop}}};
        const std::array routes{RouteEntry{IpVersion::v4, {}, 0, 0}, RouteEntry{IpVersion::v6, {}, 0, 0}};
        SnapshotStore store;
        if (!store.publish(std::make_shared<const RoutingSnapshot>(1, routes, groups)))
            throw std::runtime_error("Initial routing publication failed");
        std::unique_ptr<RoutingService> routing;
        if (control_path) routing = std::make_unique<RoutingService>(store, *control_path,
                                                                    output.interface_id(), control_timeout);
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
                if (routing) routing->stop();
                for (auto& worker : processing.workers) worker->close();
                closed = true;
            }
            for (std::size_t i = 0; i < count; ++i) {
                if (!transmit[i]) transmit[i] = processing.workers[i]->try_receive();
                if (!transmit[i]) continue;
                const auto* result = std::get_if<ForwardedPacket>(&transmit[i]->result);
                if (result && result->resolved.next_hop.interface_id != output.interface_id()) result = nullptr;
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
        std::cout << "received=" << received << " forwarded=" << forwarded << " dropped=" << dropped
                  << " routing_version=" << store.acquire()->version() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
