#include "forwarding/control.hpp"

#include <cstdlib>
#include <iostream>

using namespace forwarding;
namespace {
void check(bool ok, const char* text) {
    if (!ok) { std::cerr << "FAIL: " << text << '\n'; std::exit(EXIT_FAILURE); }
}
UpdateBatch initial() {
    return {0, 1, {{RouteEntry{IpVersion::v4, {}, 0, 7}, false}},
            {{7, {NextHop{7, 1, {}, {}}}}}};
}
void boundaries() {
    SnapshotStore store;
    ControlPlane<1> control(store);
    check(control.try_submit(initial()), "submit initial batch");
    check(control.poll() && store.acquire()->version() == 1, "apply initial batch");
    check(control.try_submit(UpdateBatch{1, 2, {}, {}}), "submit second batch");
    check(!control.poll() && store.acquire()->version() == 2, "ack backpressure");
    check(!control.poll() && store.acquire()->version() == 2, "must not repeat publication");
    auto ack = control.try_receive();
    check(ack && ack->version == 1 && ack->status == UpdateStatus::applied, "initial ack");
    check(control.poll(), "release pending ack");
    ack = control.try_receive();
    check(ack && ack->version == 2 && ack->status == UpdateStatus::applied, "second ack");
    auto bad = UpdateBatch{2, 3, {{RouteEntry{IpVersion::v4, {}, 33, 7}, false}}, {}};
    check(control.try_submit(std::move(bad)) && control.poll(), "process invalid batch");
    ack = control.try_receive();
    check(ack && ack->status == UpdateStatus::invalid && store.acquire()->version() == 2,
          "invalid batch must roll back");
    check(control.try_submit(UpdateBatch{1, 3, {}, {}}) && control.poll(), "process stale batch");
    ack = control.try_receive();
    check(ack && ack->status == UpdateStatus::stale_base, "stale batch ack");
    control.close();
    auto retained = initial();
    check(!control.try_submit(std::move(retained)) && !retained.routes.empty(), "closed ownership");
    control.run();
    check(control.finished(), "control shutdown");

    const auto stale = std::make_shared<const RoutingSnapshot>(
        10, *store.acquire(), std::span<const RouteUpdate>{}, std::span<const NextHopGroup>{});
    check(store.publish(std::make_shared<const RoutingSnapshot>(
        3, *store.acquire(), std::span<const RouteUpdate>{}, std::span<const NextHopGroup>{})),
        "external publication");
    check(!store.publish_if_base(stale, 2) && store.acquire()->version() == 3,
          "stale base cannot overwrite intervening update");
}
void asynchronous() {
    SnapshotStore store;
    ControlPlane<3> control(store);
    std::thread thread([&] { control.run(); });
    constexpr std::uint64_t count = 1000;
    std::uint64_t sent = 0, received = 0;
    while (received < count) {
        if (sent < count) {
            auto batch = sent == 0 ? initial() : UpdateBatch{sent, sent + 1, {}, {}};
            if (control.try_submit(std::move(batch))) {
                if (++sent == count) control.close();
            }
        }
        if (auto ack = control.try_receive()) {
            check(ack->version == ++received && ack->status == UpdateStatus::applied,
                  "ordered asynchronous acknowledgments");
        }
        std::this_thread::yield();
    }
    thread.join();
    check(control.finished() && store.acquire()->version() == count, "async final state");
}
}
int main() { boundaries(); asynchronous(); std::cout << "All control-plane tests passed\n"; }
