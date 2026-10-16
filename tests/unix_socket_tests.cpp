#include "forwarding/unix_socket.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <system_error>
#include <thread>
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
        char pattern[] = "/tmp/pfe-socket-XXXXXX";
        const auto result = mkdtemp(pattern);
        if (!result) throw std::system_error(errno, std::generic_category(), "mkdtemp");
        path = result;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
void endpoint() {
    Directory directory;
    const auto path = directory.path + "/routing.sock";
    {
        ipc::UnixListener listener(path);
        struct stat metadata{};
        check(lstat(path.c_str(), &metadata) == 0 && (metadata.st_mode & 0777) == 0600, "socket permissions");
        check(!listener.try_accept(), "accept must be nonblocking");
        auto client = ipc::UnixConnection::connect(path);
        auto server = listener.try_accept();
        check(server.has_value(), "accept connected client");
        ipc::send_batch(client.get(), UpdateBatch{0, 1, {}, {}});
        check(ipc::receive_batch(server->get()).version == 1, "endpoint batch transfer");
        ipc::send_ack(server->get(), UpdateAck{1, 1, UpdateStatus::applied});
        check(ipc::receive_ack(client.get()).status == UpdateStatus::applied, "endpoint acknowledgment");
        bool refused = false;
        try { ipc::UnixListener duplicate(path); } catch (const std::system_error&) { refused = true; }
        check(refused && std::filesystem::exists(path), "do not replace existing socket");
    }
    check(!std::filesystem::exists(path), "socket cleanup");
    {
        std::ofstream(path) << "preserve";
        bool refused = false;
        try { ipc::UnixListener existing(path); } catch (const std::system_error&) { refused = true; }
        check(refused && std::filesystem::is_regular_file(path), "do not remove existing file");
        std::filesystem::remove(path);
    }
    {
        ipc::UnixListener listener(path);
        check(unlink(path.c_str()) == 0, "replace socket path fixture");
        std::ofstream(path) << "replacement";
    }
    check(std::filesystem::is_regular_file(path), "destructor removed someone else's path");
    std::filesystem::remove(path);
    check(chmod(directory.path.c_str(), 0755) == 0, "directory permission fixture");
    bool refused = false;
    try { ipc::UnixListener insecure(path); } catch (const std::invalid_argument&) { refused = true; }
    check(refused, "reject nonprivate directory");
    check(chmod(directory.path.c_str(), 0700) == 0, "restore directory");
}
void deadlines() {
    std::array<int, 2> sockets{};
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) == 0, "timeout socketpair");
    ipc::UnixConnection first(sockets[0]), second(sockets[1]);
    const std::array<std::uint8_t, 4> prefix{0, 0, 0, 32};
    check(send(first.get(), prefix.data(), prefix.size(), 0) == 4, "partial message fixture");
    bool timed_out = false;
    try { (void)ipc::receive_batch(second.get(), {20ms, {}}); }
    catch (const std::system_error& error) { timed_out = error.code().value() == ETIMEDOUT; }
    check(timed_out, "prefix and payload require a bounded deadline");

    std::stop_source stop;
    std::atomic<int> outcome{0};
    std::thread receiver([&] {
        try { (void)ipc::receive_batch(second.get(), {5s, stop.get_token()}); }
        catch (const std::system_error& error) { outcome.store(error.code().value()); }
    });
    stop.request_stop();
    receiver.join();
    check(outcome.load() == ECANCELED, "cancel blocked socket read");
}
}
int main() { endpoint(); deadlines(); std::cout << "All Unix socket tests passed\n"; }
