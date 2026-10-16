#include "forwarding/routing_request.hpp"

#include <cstdlib>
#include <iostream>

using namespace forwarding;
namespace {
void check(bool value, const char* text) {
    if (!value) { std::cerr << "FAIL: " << text << '\n'; std::exit(EXIT_FAILURE); }
}
RoutingRequest parse(std::initializer_list<std::string_view> arguments) {
    return parse_routing_request(std::span(arguments.begin(), arguments.size()));
}
void invalid(std::initializer_list<std::string_view> arguments) {
    bool failed = false;
    try { (void)parse(arguments); } catch (const std::invalid_argument&) { failed = true; }
    check(failed, "invalid client command accepted");
}
}
int main() {
    const auto v4 = parse({"/tmp/control.sock", "1", "2", "upsert", "192.0.2.7/24", "0"});
    check(v4.batch.routes[0].route.prefix[3] == 7 && v4.batch.routes[0].route.prefix_length == 24 &&
          v4.batch.routes[0].route.next_hop_group == 0, "IPv4 prefix/group parsing");
    const auto v6 = parse({"socket", "18446744073709551614", "18446744073709551615", "erase", "2001:db8::1/128"});
    check(v6.batch.version == UINT64_MAX && v6.batch.routes[0].remove &&
          v6.batch.routes[0].route.family == IpVersion::v6 && v6.batch.routes[0].route.prefix[15] == 1,
          "IPv6 and full-width versions");
    const auto group = parse({"socket", "0", "1", "group", "7", "42", "02:ab:00:ff:01:02", "02:01:02:03:04:05", "0"});
    check(!group.batch.groups[0].members[0].available &&
          group.batch.groups[0].members[0].source_mac[1] == 0xab, "group parsing");
    invalid({});
    invalid({"socket", "-1", "2", "erase", "0.0.0.0/0"});
    invalid({"socket", "1", "1", "erase", "0.0.0.0/0"});
    invalid({"socket", "1", "2", "erase", "192.0.2.1/33"});
    invalid({"socket", "1", "2", "erase", "2001:db8::1/129"});
    invalid({"socket", "1", "2", "erase", "bad/24"});
    invalid({"socket", "1", "2", "erase", "192.0.2.1"});
    invalid({"socket", "1", "2", "group", "7", "42", "02-ab:00:ff:01:02", "02:01:02:03:04:05"});
    invalid({"socket", "1", "2", "group", "7", "42", "02:ab:00:ff:01:02", "02:01:02:03:04:05", "2"});
    std::cout << "All routing request tests passed\n";
}
