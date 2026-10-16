#include "forwarding/linux_io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace forwarding {
namespace {
[[noreturn]] void io_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}
}

PacketSocket::PacketSocket(std::string_view interface) {
    if (interface.empty() || interface.size() >= IFNAMSIZ || interface.find('\0') != interface.npos)
        throw std::invalid_argument("Invalid interface name");
    const std::string name(interface);
    interface_id_ = if_nametoindex(name.c_str());
    if (interface_id_ == 0) io_error("if_nametoindex");
    if (interface_id_ > static_cast<unsigned>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("Interface index exceeds socket representation");
    socket_ = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(ETH_P_ALL));
    if (socket_ < 0) io_error("AF_PACKET socket");
    try {
        ifreq request{};
        std::memcpy(request.ifr_name, name.c_str(), name.size() + 1);
        if (ioctl(socket_, SIOCGIFMTU, &request) < 0) io_error("SIOCGIFMTU");
        if (request.ifr_mtu <= 0) throw std::invalid_argument("Invalid interface MTU");
        mtu_ = static_cast<std::size_t>(request.ifr_mtu);
        if (ioctl(socket_, SIOCGIFHWADDR, &request) < 0) io_error("SIOCGIFHWADDR");
        std::memcpy(mac_.data(), request.ifr_hwaddr.sa_data, mac_.size());
        sockaddr_ll address{};
        address.sll_family = AF_PACKET;
        address.sll_protocol = htons(ETH_P_ALL);
        address.sll_ifindex = static_cast<int>(interface_id_);
        if (bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0)
            io_error("AF_PACKET bind");
    } catch (...) {
        close(socket_);
        socket_ = -1;
        throw;
    }
}

PacketSocket::~PacketSocket() { if (socket_ >= 0) close(socket_); }

std::optional<std::size_t> PacketSocket::receive(std::span<std::uint8_t> buffer) {
    sockaddr_ll address{};
    socklen_t length = sizeof(address);
    ssize_t size;
    do {
        size = recvfrom(socket_, buffer.data(), buffer.size(), MSG_TRUNC,
                        reinterpret_cast<sockaddr*>(&address), &length);
    } while (size < 0 && errno == EINTR);
    if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return std::nullopt;
    if (size < 0) io_error("AF_PACKET receive");
    if (address.sll_pkttype == PACKET_OUTGOING) return std::nullopt;
    if (static_cast<std::size_t>(size) > buffer.size()) throw std::length_error("Truncated Ethernet frame");
    return static_cast<std::size_t>(size);
}

bool PacketSocket::transmit(std::span<const std::uint8_t> frame) {
    if (frame.size() < ETH_HLEN || frame.size() - ETH_HLEN > mtu_)
        throw std::length_error("Frame violates Ethernet header or output MTU");
    std::array<std::uint8_t, ETH_ZLEN> padded{};
    if (frame.size() < padded.size()) {
        std::copy(frame.begin(), frame.end(), padded.begin());
        frame = padded;
    }
    sockaddr_ll address{};
    address.sll_family = AF_PACKET;
    address.sll_ifindex = static_cast<int>(interface_id_);
    address.sll_protocol = htons(static_cast<std::uint16_t>((static_cast<unsigned>(frame[12]) << 8U) | frame[13]));
    address.sll_halen = ETH_ALEN;
    std::copy_n(frame.begin(), ETH_ALEN, address.sll_addr);
    ssize_t sent;
    do {
        sent = sendto(socket_, frame.data(), frame.size(), MSG_DONTWAIT,
                      reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    } while (sent < 0 && errno == EINTR);
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) return false;
    if (sent < 0) io_error("AF_PACKET transmit");
    if (static_cast<std::size_t>(sent) != frame.size()) throw std::runtime_error("Short Ethernet transmit");
    return true;
}

void pin_current_thread(unsigned cpu) {
    if (cpu >= CPU_SETSIZE) throw std::out_of_range("CPU exceeds affinity mask");
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    const int result = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (result != 0) throw std::system_error(result, std::generic_category(), "pthread_setaffinity_np");
}

} // namespace forwarding
