#include "forwarding/ipc.hpp"

#include <cstdlib>
#include <iostream>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace forwarding;
namespace {
void check(bool ok, const char* text) {
    if (!ok) { std::cerr << "FAIL: " << text << '\n'; std::exit(EXIT_FAILURE); }
}
UpdateBatch batch() {
    return {0, 1, {{RouteEntry{IpVersion::v4, {192, 0, 2, 1}, 24, 7}, false},
                   {RouteEntry{IpVersion::v6, {0x20, 1}, 128, 7}, true}},
            {{7, {NextHop{3, 5, {2, 1, 2, 3, 4, 5}, {2, 5, 4, 3, 2, 1}, false}}}}};
}
void codec() {
    const auto original = batch();
    const auto bytes = ipc::encode(original);
    const auto decoded = ipc::decode(bytes);
    check(ipc::encode(decoded) == bytes, "wire roundtrip");
    check(decoded.routes[0].route.prefix == original.routes[0].route.prefix &&
          decoded.routes[1].remove && decoded.groups[0].members == original.groups[0].members,
          "wire field preservation");
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        bool rejected = false;
        try { (void)ipc::decode(std::span(bytes).first(length)); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "truncated IPC accepted");
    }
    for (const auto offset : {0, 5, 7, 24, 32, 33, 34, 35}) {
        auto corrupt = bytes;
        corrupt[static_cast<std::size_t>(offset)] = 0xff;
        bool rejected = false;
        try { (void)ipc::decode(corrupt); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "invalid IPC field accepted");
    }
    auto trailing = bytes;
    trailing.push_back(0);
    bool rejected = false;
    try { (void)ipc::decode(trailing); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "trailing IPC accepted");
}
void process_transport() {
    std::array<int, 2> sockets{};
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) == 0, "Unix socketpair");
    const auto child = fork();
    check(child >= 0, "fork routing process");
    if (child == 0) {
        close(sockets[0]);
        try {
            ipc::send_batch(sockets[1], batch());
            const auto ack = ipc::receive_ack(sockets[1]);
            _exit(ack.status == UpdateStatus::applied && ack.version == 1 ? 0 : 1);
        } catch (...) { _exit(2); }
    }
    close(sockets[1]);
    SnapshotStore store;
    ControlPlane<1> control(store);
    check(control.try_submit(ipc::receive_batch(sockets[0])) && control.poll(), "IPC batch application");
    const auto ack = control.try_receive();
    check(ack.has_value(), "IPC acknowledgment");
    ipc::send_ack(sockets[0], *ack);
    close(sockets[0]);
    int status = 0;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "routing process exchange");
    check(store.acquire()->version() == 1, "IPC publication version");
}
}
int main() { codec(); process_transport(); std::cout << "All IPC tests passed\n"; }
