#include "forwarding/worker.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>

using namespace forwarding;

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

const std::array hops{
    NextHop{1, 11, {2, 0, 0, 0, 0, 1}, {2, 0, 0, 0, 1, 1}},
    NextHop{2, 12, {2, 0, 0, 0, 0, 2}, {2, 0, 0, 0, 1, 2}}};

PacketJob job(std::uint64_t id) {
    const bool v4 = id % 2 == 0;
    std::vector<std::uint8_t> frame(v4 ? 46 : 66, 0);
    frame[12] = v4 ? 8 : 0x86;
    frame[13] = v4 ? 0 : 0xdd;
    frame[14] = v4 ? 0x45 : 0x60;
    if (v4) {
        frame[17] = 32;
        frame[22] = id % 5 == 1 ? 1 : 64;
        frame[23] = 17;
        frame[26] = 192;
        frame[29] = 1;
        frame[30] = 198;
        frame[33] = 2;
        unsigned sum = 0;
        for (std::size_t i = 14; i < 34; i += 2) {
            sum += static_cast<unsigned>(frame[i]) * 256 + frame[i + 1];
            sum = (sum & 65535U) + sum / 65536U;
        }
        const auto checksum = static_cast<std::uint16_t>(~sum);
        frame[24] = static_cast<std::uint8_t>(checksum >> 8U);
        frame[25] = static_cast<std::uint8_t>(checksum & 255U);
    } else {
        frame[19] = 12;
        frame[20] = 17;
        frame[21] = id % 5 == 1 ? 1 : 64;
        frame[22] = frame[38] = 0x20;
        frame[37] = 1;
        frame[53] = 2;
    }
    frame.back() = static_cast<std::uint8_t>(id);
    if (id % 5 == 0) {
        frame.resize(10);
    }
    return PacketJob{id, std::move(frame)};
}

void configure(Fib& fib, NextHopTable& table) {
    check(fib.insert(IpVersion::v4, {}, 0, 1), "install IPv4 route");
    check(fib.insert(IpVersion::v6, {}, 0, 1), "install IPv6 route");
    check(table.upsert(1, hops), "install ECMP group");
}

void verify(const PacketCompletion& completion, const Fib& fib, const NextHopTable& table) {
    auto expected = job(completion.id);
    const auto direct = forward_packet(expected.frame, fib, table);
    check(completion.frame == expected.frame, "worker frame differs from direct forwarding");
    check(completion.result.index() == direct.index(), "worker result category");
    if (const auto* success = std::get_if<ForwardedPacket>(&direct)) {
        const auto& actual = std::get<ForwardedPacket>(completion.result);
        check(actual.resolved == success->resolved && actual.frame_length == success->frame_length,
              "worker output adjacency or length");
    } else if (const auto* parse = std::get_if<ParseError>(&direct)) {
        check(std::get<ParseError>(completion.result) == *parse, "worker parse error");
    } else if (const auto* route = std::get_if<ResolutionError>(&direct)) {
        check(std::get<ResolutionError>(completion.result) == *route, "worker resolution error");
    } else {
        check(std::get<ForwardError>(completion.result) == std::get<ForwardError>(direct),
              "worker forwarding error");
    }
}

void backpressure() {
    Fib fib;
    NextHopTable table;
    configure(fib, table);
    ForwardingWorker<1> worker(fib, table);
    check(worker.poll() == WorkerProgress::idle, "idle worker");
    auto first = job(2);
    auto second = job(3);
    auto third = job(4);
    auto fourth = job(7);
    const auto* owned_buffer = first.frame.data();
    check(worker.try_submit(std::move(first)), "first submission");
    check(worker.poll() == WorkerProgress::progressed, "publish first completion");
    check(worker.try_submit(std::move(second)), "second submission");
    check(worker.poll() == WorkerProgress::backpressure, "full output backpressure");
    check(worker.try_submit(std::move(third)), "input slot while pending");
    const auto saved = fourth.frame;
    check(!worker.try_submit(std::move(fourth)) && fourth.frame == saved,
          "full input must preserve ownership");
    worker.close();
    worker.close();
    check(!worker.try_submit(std::move(fourth)) && fourth.frame == saved,
          "closed input must preserve ownership");
    for (unsigned retry = 0; retry < 100; ++retry) {
        check(worker.poll() == WorkerProgress::backpressure, "pending completion lost");
    }
    check(!worker.finished(), "must not finish while backpressured");
    auto completion = worker.try_receive();
    check(completion && completion->id == 2, "first completion order");
    check(completion->frame.data() == owned_buffer, "packet buffer was copied");
    verify(*completion, fib, table);
    for (const auto id : {3, 4}) {
        check(worker.poll() == WorkerProgress::progressed, "resume after backpressure");
        completion = worker.try_receive();
        check(completion && completion->id == static_cast<std::uint64_t>(id), "drain order");
        verify(*completion, fib, table);
    }
    check(worker.poll() == WorkerProgress::stopped && worker.finished(), "graceful stop");
    check(worker.poll() == WorkerProgress::stopped && !worker.try_receive(), "stop is stable");

    ForwardingWorker<1> empty(fib, table);
    empty.close();
    empty.run();
    check(empty.finished() && !empty.try_receive(), "empty shutdown");

    ForwardingWorker<1> queued(fib, table);
    check(queued.try_submit(job(9)), "submit final completion");
    queued.close();
    queued.run(); // One completion fits even without a concurrent consumer.
    completion = queued.try_receive();
    check(queued.finished() && completion && completion->id == 9, "finished retains completion");
    verify(*completion, fib, table);

    Fib missing;
    ForwardingWorker<1> unresolved(missing, table);
    check(unresolved.try_submit(job(2)), "unresolved job submission");
    unresolved.close();
    unresolved.run();
    completion = unresolved.try_receive();
    check(completion.has_value(), "missing unresolved completion");
    verify(*completion, missing, table);
}

void multicore() {
    Fib fib;
    NextHopTable table;
    configure(fib, table);
    constexpr std::size_t workers = 8;
    constexpr std::size_t per_worker = 2000;
    std::array<std::unique_ptr<ForwardingWorker<3>>, workers> pool;
    std::array<std::thread, workers> threads;
    std::array<std::size_t, workers> submitted{};
    std::array<std::size_t, workers> received{};
    for (std::size_t i = 0; i < workers; ++i) {
        pool[i] = std::make_unique<ForwardingWorker<3>>(fib, table);
        threads[i] = std::thread([&, i] { pool[i]->run(); });
    }
    std::size_t total = 0;
    while (total < workers * per_worker) {
        for (std::size_t i = 0; i < workers; ++i) {
            if (submitted[i] < per_worker) {
                auto packet = job(i * per_worker + submitted[i]);
                if (pool[i]->try_submit(std::move(packet))) {
                    if (++submitted[i] == per_worker) {
                        pool[i]->close();
                    }
                }
            }
            if (const auto completion = pool[i]->try_receive()) {
                check(completion->id == i * per_worker + received[i], "per-worker order/duplication");
                verify(*completion, fib, table);
                ++received[i];
                ++total;
            }
        }
        std::this_thread::yield();
    }
    for (std::size_t i = 0; i < workers; ++i) {
        threads[i].join();
        check(pool[i]->finished() && !pool[i]->try_receive(), "worker drain after join");
        check(submitted[i] == per_worker && received[i] == per_worker, "worker lost jobs");
    }
}
} // namespace

int main() {
    backpressure();
    multicore();
    std::cout << "All forwarding worker tests passed\n";
}
