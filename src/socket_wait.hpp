#pragma once

#include "forwarding/ipc.hpp"

#include <algorithm>
#include <cerrno>
#include <poll.h>
#include <system_error>

namespace forwarding::ipc::detail {
using Clock = std::chrono::steady_clock;

inline Clock::time_point deadline(IoOptions options) {
    if (options.timeout.count() <= 0 || options.timeout > std::chrono::hours(1))
        throw std::invalid_argument("IPC timeout must be in (0, 1 hour]");
    return Clock::now() + options.timeout;
}

inline void wait_ready(int socket, short events, Clock::time_point until, std::stop_token stop) {
    for (;;) {
        if (stop.stop_requested()) throw std::system_error(ECANCELED, std::generic_category(), "IPC canceled");
        const auto now = Clock::now();
        if (now >= until) throw std::system_error(ETIMEDOUT, std::generic_category(), "IPC deadline");
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(until - now).count();
        const auto timeout = static_cast<int>(std::min<decltype(remaining)>(remaining, 20));
        pollfd descriptor{socket, events, 0};
        const int result = poll(&descriptor, 1, timeout);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) throw std::system_error(errno, std::generic_category(), "IPC poll");
        if (descriptor.revents & POLLNVAL) throw std::system_error(EBADF, std::generic_category(), "IPC descriptor");
        if (descriptor.revents & (events | POLLHUP | POLLERR)) return;
    }
}
} // namespace forwarding::ipc::detail
