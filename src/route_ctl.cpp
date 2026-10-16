#include "forwarding/routing_request.hpp"
#include "forwarding/unix_socket.hpp"

#include <iostream>
#include <system_error>

namespace {
void usage() {
    std::cout << "Usage: route_ctl SOCKET BASE_VERSION NEW_VERSION COMMAND ...\n"
                 "  upsert IP/PREFIX GROUP\n  erase IP/PREFIX\n"
                 "  group GROUP INTERFACE_ID SOURCE_MAC DEST_MAC [AVAILABLE=1]\n";
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") { usage(); return 0; }
    try {
        std::vector<std::string_view> arguments;
        for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
        auto request = forwarding::parse_routing_request(arguments);
        auto connection = forwarding::ipc::UnixConnection::connect(request.socket_path);
        forwarding::ipc::send_batch(connection.get(), request.batch);
        const auto ack = forwarding::ipc::receive_ack(connection.get());
        if (ack.version != request.batch.version) throw std::runtime_error("Acknowledgment version mismatch");
        constexpr std::array names{"applied", "stale_base", "invalid", "resource_exhausted"};
        std::cout << "{\"version\":" << ack.version << ",\"current_version\":" << ack.current_version
                  << ",\"status\":\"" << names[static_cast<std::size_t>(ack.status)] << "\"}\n";
        return ack.status == forwarding::UpdateStatus::applied ? 0 : 1;
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << '\n'; usage(); return 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 3;
    }
}
