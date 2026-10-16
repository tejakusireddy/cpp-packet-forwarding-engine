#pragma once

#include "forwarding/ipc.hpp"

#include <optional>
#include <string>

namespace forwarding::ipc {

class UnixConnection {
public:
    explicit UnixConnection(int socket) noexcept : socket_(socket) {}
    ~UnixConnection();
    UnixConnection(const UnixConnection&) = delete;
    UnixConnection& operator=(const UnixConnection&) = delete;
    UnixConnection(UnixConnection&& other) noexcept;
    UnixConnection& operator=(UnixConnection&& other) noexcept;
    [[nodiscard]] int get() const noexcept { return socket_; }
    [[nodiscard]] int release() noexcept;
    [[nodiscard]] static UnixConnection connect(const std::string& path, IoOptions options = {});
private:
    int socket_;
};

// Owns one listening socket and its filesystem entry. The parent directory must
// be owned by this effective UID with no group/other permissions. Existing paths
// are never removed at startup. Backlog is four; accept is nonblocking.
class UnixListener {
public:
    explicit UnixListener(std::string path);
    ~UnixListener();
    UnixListener(const UnixListener&) = delete;
    UnixListener& operator=(const UnixListener&) = delete;
    [[nodiscard]] std::optional<UnixConnection> try_accept();
private:
    std::string path_;
    int socket_ = -1;
    std::uint64_t device_ = 0, inode_ = 0;
    void remove_owned_path() noexcept;
};

} // namespace forwarding::ipc
