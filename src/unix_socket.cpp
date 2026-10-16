#include "forwarding/unix_socket.hpp"
#include "socket_wait.hpp"

#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace forwarding::ipc {
namespace {
[[noreturn]] void error(const char* text) { throw std::system_error(errno, std::generic_category(), text); }
sockaddr_un address(const std::string& path) {
    sockaddr_un result{};
    if (path.empty() || path.size() >= sizeof(result.sun_path) || path.find('\0') != path.npos)
        throw std::invalid_argument("Invalid Unix socket path");
    result.sun_family = AF_UNIX;
    std::memcpy(result.sun_path, path.c_str(), path.size() + 1);
    return result;
}
void configure(int socket) {
    if (fcntl(socket, F_SETFD, FD_CLOEXEC) < 0 || fcntl(socket, F_SETFL, O_NONBLOCK) < 0)
        error("Unix socket flags");
}
UnixConnection open_socket() {
    const int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket < 0) error("Unix socket");
    UnixConnection connection(socket);
    configure(socket);
    return connection;
}
} // namespace

UnixConnection::~UnixConnection() { if (socket_ >= 0) close(socket_); }
int UnixConnection::release() noexcept { return std::exchange(socket_, -1); }
UnixConnection::UnixConnection(UnixConnection&& other) noexcept : socket_(std::exchange(other.socket_, -1)) {}
UnixConnection& UnixConnection::operator=(UnixConnection&& other) noexcept {
    if (this != &other) {
        if (socket_ >= 0) close(socket_);
        socket_ = std::exchange(other.socket_, -1);
    }
    return *this;
}

UnixConnection UnixConnection::connect(const std::string& path, IoOptions options) {
    const auto endpoint = address(path);
    const auto until = detail::deadline(options);
    auto connection = open_socket();
    if (::connect(connection.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) < 0) {
        if (errno != EINPROGRESS) error("Unix connect");
        detail::wait_ready(connection.get(), POLLOUT, until, options.stop);
        int status = 0;
        socklen_t size = sizeof(status);
        if (getsockopt(connection.get(), SOL_SOCKET, SO_ERROR, &status, &size) < 0) error("Unix connect status");
        if (status != 0) throw std::system_error(status, std::generic_category(), "Unix connect");
    }
    return connection;
}

UnixListener::UnixListener(std::string path) : path_(std::move(path)) {
    const auto endpoint = address(path_);
    auto parent = std::filesystem::path(path_).parent_path();
    if (parent.empty()) parent = ".";
    struct stat directory{};
    if (lstat(parent.c_str(), &directory) < 0) error("Socket directory");
    if (!S_ISDIR(directory.st_mode) || directory.st_uid != geteuid() || (directory.st_mode & 077) != 0)
        throw std::invalid_argument("Socket directory must be owned by this UID and private (0700)");
    auto connection = open_socket();
    if (bind(connection.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) < 0)
        error("Unix bind (existing paths are not removed)");
    struct stat entry{};
    if (lstat(path_.c_str(), &entry) < 0) error("Bound socket metadata");
    device_ = static_cast<std::uint64_t>(entry.st_dev);
    inode_ = static_cast<std::uint64_t>(entry.st_ino);
    try {
        if (chmod(path_.c_str(), 0600) < 0 || listen(connection.get(), 4) < 0) error("Unix listen");
    } catch (...) { remove_owned_path(); throw; }
    // Transfer the descriptor only after the full constructor has succeeded.
    socket_ = connection.release();
}

void UnixListener::remove_owned_path() noexcept {
    struct stat entry{};
    if (lstat(path_.c_str(), &entry) == 0 && S_ISSOCK(entry.st_mode) &&
        static_cast<std::uint64_t>(entry.st_dev) == device_ && static_cast<std::uint64_t>(entry.st_ino) == inode_)
        (void)unlink(path_.c_str());
}
UnixListener::~UnixListener() { if (socket_ >= 0) close(socket_); remove_owned_path(); }

std::optional<UnixConnection> UnixListener::try_accept() {
    const int socket = accept(socket_, nullptr, nullptr);
    if (socket < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return std::nullopt;
    if (socket < 0) error("Unix accept");
    UnixConnection connection(socket);
    configure(socket);
    return connection;
}

} // namespace forwarding::ipc
