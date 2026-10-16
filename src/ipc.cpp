#include "forwarding/ipc.hpp"
#include "socket_wait.hpp"

#include <cerrno>
#include <system_error>
#include <sys/socket.h>

namespace forwarding::ipc {
namespace {
struct Writer {
    std::vector<std::uint8_t> bytes;
    void put(std::uint64_t value, unsigned width) {
        if (bytes.size() + width > max_message_size) throw std::invalid_argument("IPC message too large");
        for (unsigned i = width; i > 0; --i) {
            bytes.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
        }
    }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t position = 0;
    std::uint64_t get(unsigned width) {
        if (width > bytes.size() - position) throw std::invalid_argument("Truncated IPC message");
        std::uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) value = (value << 8U) | bytes[position++];
        return value;
    }
    void zero(unsigned width) {
        if (get(width) != 0) throw std::invalid_argument("Nonzero reserved IPC field");
    }
};
void header(Writer& writer, std::uint32_t magic) {
    writer.put(magic, 4); writer.put(1, 2); writer.put(0, 2);
}
void header(Reader& reader, std::uint32_t magic) {
    if (reader.get(4) != magic || reader.get(2) != 1) throw std::invalid_argument("Unsupported IPC protocol");
    reader.zero(2);
}
void transfer(int socket, std::span<std::uint8_t> bytes, bool writing,
              detail::Clock::time_point until, std::stop_token stop) {
#if defined(SO_NOSIGPIPE)
    if (writing) {
        const int enabled = 1;
        if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
            throw std::system_error(errno, std::generic_category(), "setsockopt");
    }
#endif
    std::size_t done = 0;
    while (done < bytes.size()) {
        detail::wait_ready(socket, writing ? POLLOUT : POLLIN, until, stop);
        ssize_t count;
        if (writing) {
#if defined(MSG_NOSIGNAL)
            constexpr int flags = MSG_NOSIGNAL | MSG_DONTWAIT;
#else
            constexpr int flags = MSG_DONTWAIT;
#endif
            count = send(socket, bytes.data() + done, bytes.size() - done, flags);
        } else {
            count = recv(socket, bytes.data() + done, bytes.size() - done, MSG_DONTWAIT);
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count < 0) throw std::system_error(errno, std::generic_category(), "IPC transfer");
        if (count == 0) throw std::system_error(ECONNRESET, std::generic_category(), "IPC peer closed");
        done += static_cast<std::size_t>(count);
    }
}
void send_message(int socket, std::vector<std::uint8_t> bytes, IoOptions options) {
    const auto until = detail::deadline(options);
    Writer length;
    length.put(bytes.size(), 4);
    transfer(socket, length.bytes, true, until, options.stop);
    transfer(socket, bytes, true, until, options.stop);
}
std::vector<std::uint8_t> receive_message(int socket, IoOptions options) {
    const auto until = detail::deadline(options);
    std::array<std::uint8_t, 4> prefix{};
    transfer(socket, prefix, false, until, options.stop);
    Reader reader{prefix};
    const auto size = reader.get(4);
    if (size == 0 || size > max_message_size) throw std::invalid_argument("Invalid IPC length");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    transfer(socket, bytes, false, until, options.stop);
    return bytes;
}
} // namespace

std::vector<std::uint8_t> encode(const UpdateBatch& batch) {
    if (batch.routes.size() > max_routes || batch.groups.size() > max_groups)
        throw std::invalid_argument("Too many IPC entries");
    Writer out;
    header(out, 0x50464531);
    out.put(batch.base_version, 8); out.put(batch.version, 8);
    out.put(batch.routes.size(), 4); out.put(batch.groups.size(), 4);
    for (const auto& change : batch.routes) {
        const auto& route = change.route;
        const auto bits = route.family == IpVersion::v4 ? 32U : route.family == IpVersion::v6 ? 128U : 0U;
        if (bits == 0 || route.prefix_length > bits) throw std::invalid_argument("Invalid IPC route");
        out.put(route.family == IpVersion::v4 ? 4 : 6, 1);
        out.put(route.prefix_length, 1); out.put(change.remove ? 1 : 0, 1); out.put(0, 1);
        out.put(route.next_hop_group, 4);
        for (const auto byte : route.prefix) out.put(byte, 1);
    }
    for (const auto& group : batch.groups) {
        if (group.members.empty() || group.members.size() > max_members)
            throw std::invalid_argument("Invalid IPC group size");
        out.put(group.id, 4); out.put(group.members.size(), 2); out.put(0, 2);
        for (const auto& member : group.members) {
            out.put(member.id, 4); out.put(member.interface_id, 4);
            for (const auto byte : member.source_mac) out.put(byte, 1);
            for (const auto byte : member.destination_mac) out.put(byte, 1);
            out.put(member.available ? 1 : 0, 1); out.put(0, 3);
        }
    }
    return std::move(out.bytes);
}

UpdateBatch decode(std::span<const std::uint8_t> message) {
    if (message.size() > max_message_size) throw std::invalid_argument("IPC message too large");
    Reader in{message};
    header(in, 0x50464531);
    UpdateBatch batch{};
    batch.base_version = in.get(8); batch.version = in.get(8);
    const auto routes = in.get(4), groups = in.get(4);
    if (routes > max_routes || groups > max_groups) throw std::invalid_argument("Too many IPC entries");
    for (std::uint64_t i = 0; i < routes; ++i) {
        const auto family = in.get(1), length = in.get(1), remove = in.get(1);
        in.zero(1);
        if ((family != 4 && family != 6) || length > (family == 4 ? 32U : 128U) || remove > 1)
            throw std::invalid_argument("Invalid IPC route");
        RouteUpdate update{{family == 4 ? IpVersion::v4 : IpVersion::v6, {},
                            static_cast<std::size_t>(length), static_cast<std::uint32_t>(in.get(4))}, remove == 1};
        for (auto& byte : update.route.prefix) byte = static_cast<std::uint8_t>(in.get(1));
        batch.routes.push_back(update);
    }
    for (std::uint64_t i = 0; i < groups; ++i) {
        NextHopGroup group{static_cast<std::uint32_t>(in.get(4)), {}};
        const auto members = in.get(2);
        in.zero(2);
        if (members == 0 || members > max_members) throw std::invalid_argument("Invalid IPC group size");
        for (std::uint64_t j = 0; j < members; ++j) {
            NextHop member{};
            member.id = static_cast<std::uint32_t>(in.get(4));
            member.interface_id = static_cast<std::uint32_t>(in.get(4));
            for (auto& byte : member.source_mac) byte = static_cast<std::uint8_t>(in.get(1));
            for (auto& byte : member.destination_mac) byte = static_cast<std::uint8_t>(in.get(1));
            const auto available = in.get(1);
            if (available > 1) throw std::invalid_argument("Invalid IPC availability");
            member.available = available == 1;
            in.zero(3);
            group.members.push_back(member);
        }
        batch.groups.push_back(std::move(group));
    }
    if (in.position != message.size()) throw std::invalid_argument("Trailing IPC data");
    return batch;
}

void send_batch(int socket, const UpdateBatch& batch, IoOptions options) { send_message(socket, encode(batch), options); }
UpdateBatch receive_batch(int socket, IoOptions options) { return decode(receive_message(socket, options)); }
void send_ack(int socket, const UpdateAck& ack, IoOptions options) {
    Writer out;
    header(out, 0x41434b31);
    out.put(ack.version, 8); out.put(ack.current_version, 8);
    out.put(static_cast<std::uint8_t>(ack.status), 1); out.put(0, 7);
    send_message(socket, std::move(out.bytes), options);
}
UpdateAck receive_ack(int socket, IoOptions options) {
    const auto message = receive_message(socket, options);
    Reader in{message};
    header(in, 0x41434b31);
    UpdateAck ack{};
    ack.version = in.get(8); ack.current_version = in.get(8);
    const auto status = in.get(1);
    if (status > static_cast<unsigned>(UpdateStatus::resource_exhausted)) throw std::invalid_argument("Invalid IPC status");
    ack.status = static_cast<UpdateStatus>(status);
    in.zero(7);
    if (in.position != message.size()) throw std::invalid_argument("Trailing IPC acknowledgment data");
    return ack;
}

} // namespace forwarding::ipc
