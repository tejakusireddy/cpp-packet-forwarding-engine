#include "forwarding/linux_io.hpp"

#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <system_error>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace forwarding;

int main() {
    try {
        PacketSocket missing("pfe-no-such");
        std::cerr << "Missing interface was accepted\n";
        return 1;
    } catch (const std::system_error&) {}
    try {
        PacketSocket receiver("lo"), sender("lo");
        std::vector<std::uint8_t> short_frame(13);
        try { (void)sender.transmit(short_frame); return 1; }
        catch (const std::length_error&) {}
        std::vector<std::uint8_t> oversized(sender.mtu() + 15);
        try { (void)sender.transmit(oversized); return 1; }
        catch (const std::length_error&) {}
        std::array<std::uint8_t, 42> frame{};
        frame[0] = frame[6] = 2;
        frame[12] = 0x88; frame[13] = 0xb5; // Local experimental EtherType.
        frame[14] = 0x50; frame[15] = 0x46;
        const auto pid = static_cast<unsigned>(getpid());
        frame[16] = static_cast<std::uint8_t>(pid >> 8U);
        frame[17] = static_cast<std::uint8_t>(pid);
        if (!sender.transmit(frame)) return 1;
        std::array<std::uint8_t, 65550> buffer{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto size = receiver.receive(buffer);
            if (size && *size == 60 && std::equal(frame.begin(), frame.end(), buffer.begin())) {
                for (std::size_t i = frame.size(); i < *size; ++i) if (buffer[i] != 0) return 1;
                std::cout << "Linux packet I/O loopback and padding passed\n";
                return 0;
            }
            std::this_thread::yield();
        }
        std::cerr << "Packet loopback timed out\n";
        return 1;
    } catch (const std::system_error& error) {
        if (error.code().value() == EPERM || error.code().value() == EACCES) {
            std::cout << "CAP_NET_RAW required\n";
            return 77;
        }
        std::cerr << error.what() << '\n';
        return 1;
    }
}
