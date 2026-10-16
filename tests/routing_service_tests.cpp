#include "forwarding/routing_service.hpp"
#include "forwarding/worker.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

using namespace forwarding;
using namespace std::chrono_literals;
namespace {
void check(bool ok, const char* text) {
    if (!ok) { std::cerr << "FAIL: " << text << '\n'; std::exit(EXIT_FAILURE); }
}
struct Directory {
    std::string path;
    Directory() {
        char pattern[] = "/tmp/pfe-service-XXXXXX";
        const auto result = mkdtemp(pattern);
        if (!result) throw std::runtime_error("mkdtemp failed");
        path = result;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
NextHop hop(std::uint32_t version, std::uint32_t interface = 42) {
    return {version, interface, {2, 0, 0, 0, 0, 1},
            {2, 0, 0, 0, 1, static_cast<std::uint8_t>(version)}};
}
UpdateAck exchange(int socket, UpdateBatch batch) {
    ipc::send_batch(socket, batch);
    return ipc::receive_ack(socket);
}
PacketJob packet(std::uint64_t id) {
    std::vector<std::uint8_t> frame(58, 0);
    frame[12] = 0x86; frame[13] = 0xdd; frame[14] = 0x60;
    frame[19] = 4; frame[20] = 17; frame[21] = 64;
    return {id, std::move(frame)};
}
void live_updates() {
    Directory directory;
    SnapshotStore store;
    const auto path = directory.path + "/routing.sock";
    {
        RoutingService service(store, path, 42, 200ms);
        auto client = ipc::UnixConnection::connect(path);
        auto ack = exchange(client.get(), {0, 1,
            {{{IpVersion::v6, {}, 0, 7}, false}}, {{7, {hop(1)}}}});
        check(ack.status == UpdateStatus::applied && store.acquire()->version() == 1, "initial remote update");
        ack = exchange(client.get(), {0, 2, {}, {}});
        check(ack.status == UpdateStatus::stale_base, "remote stale base");
        ack = exchange(client.get(), {1, 2, {{{IpVersion::v6, {}, 0, 99}, false}}, {}});
        check(ack.status == UpdateStatus::invalid && store.acquire()->version() == 1, "remote rollback");
        ack = exchange(client.get(), {1, 2, {}, {{7, {hop(2, 43)}}}});
        check(ack.status == UpdateStatus::invalid && store.acquire()->version() == 1, "reject unconfigured egress");

        ForwardingWorker<8> worker(store);
        std::thread processing([&] { worker.run(); });
        for (std::uint32_t version = 2; version <= 32; ++version) {
            ack = exchange(client.get(), {version - 1, version, {}, {{7, {hop(version)}}}});
            check(ack.status == UpdateStatus::applied, "live group publication");
            check(worker.try_submit(packet(version)), "submit during live control service");
            std::optional<PacketCompletion> result;
            while (!(result = worker.try_receive())) std::this_thread::yield();
            const auto& forwarded = std::get<ForwardedPacket>(result->result);
            check(result->routing_version == version && forwarded.resolved.next_hop.id == version &&
                  result->frame[5] == version && result->frame[21] == 63, "live forwarding version/adjacency");
        }
        worker.close(); processing.join();
        client = ipc::UnixConnection(-1);
        auto malformed = ipc::UnixConnection::connect(path);
        const std::array<std::uint8_t, 4> oversized{0, 0x20, 0, 0};
        check(send(malformed.get(), oversized.data(), oversized.size(), 0) == 4, "malformed request fixture");
        bool closed = false;
        try { (void)ipc::receive_ack(malformed.get()); } catch (const std::system_error&) { closed = true; }
        check(closed && store.acquire()->version() == 32, "malformed peer changed state");
        auto stalled = ipc::UnixConnection::connect(path);
        service.stop(); // Cancellation must not wait for a complete message.
        service.stop();
    }
    check(!std::filesystem::exists(path), "service removes owned socket");
}
void timeout_recovery() {
    Directory directory;
    SnapshotStore store;
    const auto path = directory.path + "/routing.sock";
    RoutingService service(store, path, {}, 20ms);
    auto stalled = ipc::UnixConnection::connect(path);
    auto next = ipc::UnixConnection::connect(path);
    auto ack = exchange(next.get(), {0, 1, {}, {}});
    check(ack.status == UpdateStatus::applied, "idle client must not block next connection indefinitely");
}
void session_limit() {
    Directory directory;
    SnapshotStore store;
    const auto path = directory.path + "/routing.sock";
    RoutingService service(store, path);
    auto client = ipc::UnixConnection::connect(path);
    for (std::uint64_t version = 1; version <= 64; ++version) {
        check(exchange(client.get(), {version - 1, version, {}, {}}).status == UpdateStatus::applied,
              "session request within limit");
    }
    bool closed = false;
    try { (void)exchange(client.get(), {64, 65, {}, {}}); }
    catch (const std::system_error&) { closed = true; }
    check(closed && store.acquire()->version() == 64, "session cap must close without applying request 65");
    auto next = ipc::UnixConnection::connect(path);
    check(exchange(next.get(), {64, 65, {}, {}}).status == UpdateStatus::applied,
          "session cap must not stop listener");
}
}
int main() { live_updates(); timeout_recovery(); session_limit(); std::cout << "All routing service tests passed\n"; }
