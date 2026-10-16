#include "forwarding/routing_request.hpp"

#include <arpa/inet.h>
#include <charconv>

namespace forwarding {
namespace {
template <typename T>
T number(std::string_view text) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("Invalid unsigned integer");
    return value;
}
RouteEntry prefix(std::string_view text) {
    const auto slash = text.find('/');
    if (slash == text.npos || slash == 0 || slash >= INET6_ADDRSTRLEN)
        throw std::invalid_argument("Expected IP_ADDRESS/PREFIX_LENGTH");
    const std::string ip(text.substr(0, slash));
    const auto bits = number<std::size_t>(text.substr(slash + 1));
    const bool v6 = ip.find(':') != ip.npos;
    RouteEntry result{v6 ? IpVersion::v6 : IpVersion::v4, {}, bits, 0};
    if (bits > (v6 ? 128U : 32U) || inet_pton(v6 ? AF_INET6 : AF_INET, ip.c_str(), result.prefix.data()) != 1)
        throw std::invalid_argument("Invalid IP prefix");
    return result;
}
MacAddress mac(std::string_view text) {
    if (text.size() != 17) throw std::invalid_argument("Expected aa:bb:cc:dd:ee:ff MAC address");
    MacAddress value{};
    for (std::size_t i = 0; i < value.size(); ++i) {
        unsigned byte = 0;
        const auto begin = text.data() + i * 3;
        const auto result = std::from_chars(begin, begin + 2, byte, 16);
        if (result.ec != std::errc{} || result.ptr != begin + 2 || byte > 255 ||
            (i != 5 && text[i * 3 + 2] != ':')) throw std::invalid_argument("Invalid MAC address");
        value[i] = static_cast<std::uint8_t>(byte);
    }
    return value;
}
} // namespace

RoutingRequest parse_routing_request(std::span<const std::string_view> arguments) {
    if (arguments.size() < 5) throw std::invalid_argument("Missing routing command arguments");
    RoutingRequest result{std::string(arguments[0]),
                          {number<std::uint64_t>(arguments[1]), number<std::uint64_t>(arguments[2]), {}, {}}};
    if (result.batch.version <= result.batch.base_version)
        throw std::invalid_argument("New version must exceed base version");
    const auto command = arguments[3];
    if ((command == "upsert" && arguments.size() == 6) || (command == "erase" && arguments.size() == 5)) {
        auto route = prefix(arguments[4]);
        if (command == "upsert") route.next_hop_group = number<std::uint32_t>(arguments[5]);
        result.batch.routes.push_back({route, command == "erase"});
    } else if (command == "group" && (arguments.size() == 8 || arguments.size() == 9)) {
        const auto group = number<std::uint32_t>(arguments[4]);
        NextHop hop{0, number<std::uint32_t>(arguments[5]), mac(arguments[6]), mac(arguments[7])};
        if (arguments.size() == 9) {
            const auto available = number<unsigned>(arguments[8]);
            if (available > 1) throw std::invalid_argument("Availability must be 0 or 1");
            hop.available = available == 1;
        }
        result.batch.groups.push_back({group, {hop}});
    } else {
        throw std::invalid_argument("Unknown command or incorrect argument count");
    }
    return result;
}

} // namespace forwarding
