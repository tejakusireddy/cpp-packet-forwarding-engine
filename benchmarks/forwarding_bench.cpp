#include "forwarding/control.hpp"
#include "forwarding/packet_pool.hpp"
#include "forwarding/worker.hpp"

#include <algorithm>
#include <barrier>
#include <charconv>
#include <chrono>
#include <iostream>
#include <numeric>
#include <string>
#include <sys/resource.h>
#include <sys/utsname.h>

using namespace forwarding;
using Clock = std::chrono::steady_clock;
namespace {
struct Options {
    std::size_t workers = 1, packets = 100000, routes = 10000, updates = 10000, batch = 256;
    std::string mode = "queued";
};
std::size_t number(const char* text) {
    const std::string value(text);
    std::size_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result == 0)
        throw std::invalid_argument("Expected a positive integer");
    return result;
}
Options options(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) throw std::invalid_argument("Missing option value");
        const std::string flag(argv[i]);
        if (flag == "--mode") result.mode = argv[i + 1];
        else if (flag == "--workers") result.workers = number(argv[i + 1]);
        else if (flag == "--packets") result.packets = number(argv[i + 1]);
        else if (flag == "--routes") result.routes = number(argv[i + 1]);
        else if (flag == "--updates") result.updates = number(argv[i + 1]);
        else if (flag == "--batch") result.batch = number(argv[i + 1]);
        else throw std::invalid_argument("Unknown benchmark option");
    }
    if ((result.mode != "core" && result.mode != "queued") || result.workers > 64 ||
        result.packets > 10000000 || result.routes > 8000000 || result.updates > 10000000 ||
        result.batch > 32768) throw std::invalid_argument("Benchmark option out of range");
    return result;
}
IpAddress address(std::size_t index) {
    IpAddress result{};
    result[0] = 10;
    result[1] = static_cast<std::uint8_t>(index >> 16U);
    result[2] = static_cast<std::uint8_t>(index >> 8U);
    result[3] = static_cast<std::uint8_t>(index);
    return result;
}
std::shared_ptr<const RoutingSnapshot> build(std::size_t count) {
    std::vector<RouteEntry> routes;
    routes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) routes.push_back({IpVersion::v4, address(i), 32, 0});
    const std::array groups{
        NextHopGroup{0, {NextHop{1, 1, {2, 0, 0, 0, 0, 1}, {2, 0, 0, 0, 1, 1}}}},
        NextHopGroup{1, {NextHop{2, 2, {2, 0, 0, 0, 0, 2}, {2, 0, 0, 0, 1, 2}}}}};
    return std::make_shared<const RoutingSnapshot>(1, routes, groups);
}
void fill(std::span<std::uint8_t> frame, std::size_t index) {
    std::fill(frame.begin(), frame.end(), 0);
    frame[0] = frame[6] = 2;
    frame[12] = 8; frame[14] = 0x45; frame[17] = 50;
    frame[22] = 64; frame[23] = 17;
    frame[26] = 192; frame[29] = 1;
    const auto destination = address(index);
    std::copy_n(destination.begin(), 4, frame.begin() + 30);
    frame[34] = static_cast<std::uint8_t>(index >> 8U);
    frame[35] = static_cast<std::uint8_t>(index);
    frame[37] = 53; frame[39] = 30;
    unsigned sum = 0;
    for (std::size_t i = 14; i < 34; i += 2) {
        sum += static_cast<unsigned>(frame[i]) * 256 + frame[i + 1];
        sum = (sum & 65535U) + sum / 65536U;
    }
    const auto checksum = static_cast<std::uint16_t>(~sum);
    frame[24] = static_cast<std::uint8_t>(checksum >> 8U);
    frame[25] = static_cast<std::uint8_t>(checksum);
}
struct Measurement {
    double seconds = 0;
    std::vector<std::int64_t> latency;
    std::size_t errors = 0;
};
Measurement core(const Options& config, const RoutingSnapshot& snapshot) {
    std::vector<std::thread> threads;
    std::vector<Measurement> results(config.workers);
    std::barrier start(static_cast<std::ptrdiff_t>(config.workers + 1));
    for (std::size_t worker = 0; worker < config.workers; ++worker) {
        results[worker].latency.reserve(config.packets / 1024 + 1);
        threads.emplace_back([&, worker] {
            std::array<std::array<std::uint8_t, 64>, 1024> templates{};
            for (std::size_t i = 0; i < templates.size(); ++i)
                fill(templates[i], (worker * 1024 + i) % config.routes);
            start.arrive_and_wait();
            for (std::size_t i = 0; i < config.packets; ++i) {
                auto frame = templates[i % templates.size()];
                const bool sample = i % 1024 == 0;
                const auto begin = sample ? Clock::now() : Clock::time_point{};
                const auto result = forward_packet(frame, snapshot.fib(), snapshot.next_hops());
                if (sample) results[worker].latency.push_back(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
                if (!std::holds_alternative<ForwardedPacket>(result) || frame[22] != 63)
                    ++results[worker].errors;
            }
        });
    }
    const auto begin = Clock::now();
    start.arrive_and_wait();
    for (auto& thread : threads) thread.join();
    Measurement total;
    total.seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    for (auto& result : results) {
        total.errors += result.errors;
        total.latency.insert(total.latency.end(), result.latency.begin(), result.latency.end());
    }
    return total;
}
Measurement queued(const Options& config, const SnapshotStore& store) {
    constexpr std::size_t ring = 4096;
    std::vector<std::unique_ptr<ForwardingWorker<1024>>> workers;
    std::vector<std::thread> threads;
    std::vector<std::size_t> sent(config.workers), received(config.workers);
    std::vector<Clock::time_point> timestamps(config.workers * ring);
    PacketPool pool(config.workers * 2050, 64);
    std::barrier start(static_cast<std::ptrdiff_t>(config.workers + 1));
    for (std::size_t i = 0; i < config.workers; ++i) {
        workers.push_back(std::make_unique<ForwardingWorker<1024>>(store));
        threads.emplace_back([&, i] { start.arrive_and_wait(); workers[i]->run(); });
    }
    Measurement result;
    result.latency.reserve(config.workers * (config.packets / 1024 + 1));
    const auto begin = Clock::now();
    start.arrive_and_wait();
    std::size_t completed = 0;
    while (completed < config.workers * config.packets) {
        for (std::size_t worker = 0; worker < config.workers; ++worker) {
            if (sent[worker] < config.packets) {
                auto buffer = pool.acquire(64);
                if (buffer) {
                    fill(*buffer, (worker * config.packets + sent[worker]) % config.routes);
                    PacketJob job{sent[worker], std::move(*buffer)};
                    const bool sample = job.id % 1024 == 0;
                    const auto timestamp = sample ? Clock::now() : Clock::time_point{};
                    if (workers[worker]->try_submit(std::move(job))) {
                        if (sample) timestamps[worker * ring + sent[worker] % ring] = timestamp;
                        if (++sent[worker] == config.packets) workers[worker]->close();
                    } else {
                        if (!pool.recycle(std::move(job.frame))) std::terminate();
                    }
                }
            }
            if (auto completion = workers[worker]->try_receive()) {
                if (completion->id % 1024 == 0) result.latency.push_back(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                        timestamps[worker * ring + completion->id % ring]).count());
                if (completion->id != received[worker] ||
                    !std::holds_alternative<ForwardedPacket>(completion->result) ||
                    completion->frame[22] != 63) ++result.errors;
                if (!pool.recycle(std::move(completion->frame))) std::terminate();
                ++received[worker]; ++completed;
            }
        }
    }
    result.seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    for (auto& thread : threads) thread.join();
    return result;
}
double updates(const Options& config, SnapshotStore& store) {
    std::vector<UpdateBatch> batches;
    std::size_t prepared = 0;
    std::uint64_t version = store.acquire()->version();
    while (prepared < config.updates) {
        UpdateBatch batch{version, version + 1, {}, {}};
        for (std::size_t i = 0; i < config.batch && prepared < config.updates; ++i, ++prepared)
            batch.routes.push_back({{IpVersion::v4, address(prepared % config.routes), 32,
                                      static_cast<std::uint32_t>((prepared / config.routes + 1) % 2)}, false});
        batches.push_back(std::move(batch)); ++version;
    }
    ControlPlane<8> control(store);
    std::thread thread([&] { control.run(); });
    std::size_t sent = 0, received = 0, errors = 0;
    const auto begin = Clock::now();
    while (received < batches.size()) {
        if (sent < batches.size() && control.try_submit(std::move(batches[sent]))) {
            if (++sent == batches.size()) control.close();
        }
        if (const auto ack = control.try_receive()) {
            if (ack->status != UpdateStatus::applied) ++errors;
            ++received;
        }
        std::this_thread::yield();
    }
    const auto seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    thread.join();
    (void)store.collect_retired();
    if (errors != 0) throw std::runtime_error("Benchmark update batch failed");
    return seconds;
}
}

int main(int argc, char** argv) {
    try {
        const auto config = options(argc, argv);
        SnapshotStore store;
        const auto build_begin = Clock::now();
        if (!store.publish(build(config.routes))) throw std::runtime_error("Initial publication failed");
        const auto build_seconds = std::chrono::duration<double>(Clock::now() - build_begin).count();
        // Warm the code/data path before timing; no warmup samples enter p99.
        for (std::size_t i = 0; i < 10000; ++i) {
            std::array<std::uint8_t, 64> frame{}; fill(frame, i % config.routes);
            const auto current = store.acquire();
            if (!std::holds_alternative<ForwardedPacket>(forward_packet(frame, current->fib(), current->next_hops())))
                throw std::runtime_error("Warmup failed");
        }
        auto measured = config.mode == "core" ? core(config, *store.acquire()) : queued(config, store);
        const auto update_seconds = updates(config, store);
        std::sort(measured.latency.begin(), measured.latency.end());
        const auto rank = (measured.latency.size() * 99 + 99) / 100;
        const auto p99 = measured.latency[rank - 1];
        utsname platform{}; (void)uname(&platform);
        rusage usage{}; (void)getrusage(RUSAGE_SELF, &usage);
        auto rss_kib = usage.ru_maxrss;
#if defined(__APPLE__)
        rss_kib /= 1024;
#endif
        std::cout << "{\n  \"os\": \"" << platform.sysname << "\",\n  \"architecture\": \"" << platform.machine
                  << "\",\n  \"mode\": \"" << config.mode << "\",\n  \"workers\": " << config.workers
                  << ",\n  \"packets\": " << config.workers * config.packets << ",\n  \"frame_bytes\": 64,\n  \"routes\": " << config.routes
                  << ",\n  \"build_seconds\": " << build_seconds << ",\n  \"forward_seconds\": " << measured.seconds
                  << ",\n  \"packets_per_second\": " << static_cast<double>(config.workers * config.packets) / measured.seconds
                  << ",\n  \"latency_samples\": " << measured.latency.size() << ",\n  \"sample_stride\": 1024,\n  \"p99_ns\": " << p99
                  << ",\n  \"errors\": " << measured.errors << ",\n  \"updates\": " << config.updates << ",\n  \"update_batch\": " << config.batch
                  << ",\n  \"update_seconds\": " << update_seconds << ",\n  \"updates_per_second\": " << static_cast<double>(config.updates) / update_seconds
                  << ",\n  \"peak_rss_kib\": " << rss_kib << "\n}\n";
        return measured.errors == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
