#include "forwarding/fib.hpp"

#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

using namespace forwarding;

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void boundaries() {
    Fib fib;
    const IpAddress address{0x80, 0xff, 0x12, 0x34, 0xff, 0xff, 0xff, 0xff,
                            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    check(!fib.lookup(IpVersion::v4, address), "empty IPv4 lookup");
    check(!fib.lookup(IpVersion::v6, address), "empty IPv6 lookup");
    for (const auto family : {IpVersion::v4, IpVersion::v6}) {
        const std::size_t bits = family == IpVersion::v4 ? 32 : 128;
        for (std::size_t length = 0; length <= bits; ++length) {
            check(fib.insert(family, address, length, static_cast<std::uint32_t>(length)),
                  "insert every prefix length");
            check(fib.lookup(family, address) == FibMatch{static_cast<std::uint32_t>(length), length},
                  "longest prefix wins");
        }
        check(!fib.insert(family, address, bits + 1, 7), "reject invalid prefix length");
        check(!fib.erase(family, address, bits + 1), "reject invalid erase length");
        for (std::size_t length = bits; length > 0; --length) {
            check(fib.erase(family, address, length), "erase every prefix length");
            check(!fib.erase(family, address, length), "reject duplicate erase");
            check(fib.lookup(family, address) ==
                      FibMatch{static_cast<std::uint32_t>(length - 1), length - 1},
                  "fallback to less specific prefix");
        }
        check(fib.erase(family, {}, 0), "erase default route using different host bits");
    }
    check(fib.size() == 0, "route count after removal");

    check(fib.insert(IpVersion::v4, address, 24, 7), "insert subnet");
    auto same_subnet = address;
    same_subnet[3] ^= 0xff;
    check(fib.insert(IpVersion::v4, same_subnet, 24, 0), "replace with zero group ID");
    check(fib.size() == 1, "replacement must not add a route");
    check(fib.lookup(IpVersion::v4, address) == FibMatch{0, 24}, "ignore host bits");
    check(!fib.lookup(IpVersion::v6, address), "separate address families");
    check(fib.insert(IpVersion::v4, address, 32, 9), "insert host route");
    check(fib.erase(IpVersion::v4, address, 24), "erase ancestor with live descendant");
    check(fib.lookup(IpVersion::v4, address) == FibMatch{9, 32}, "preserve descendant");
    check(!fib.lookup(IpVersion::v4, same_subnet), "removed subnet must not match");
    check(fib.erase(IpVersion::v4, address, 32), "erase host route");
    check(!fib.lookup(IpVersion::v4, address), "no match after pruning");
}

struct Route {
    IpAddress prefix;
    std::size_t length;
    std::uint32_t group;
};

// Independent byte-mask reference implementation, used only by tests.
bool matches(const IpAddress& a, const IpAddress& b, std::size_t length) {
    const auto whole = length / 8;
    for (std::size_t i = 0; i < whole; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    const auto remaining = length % 8;
    if (remaining == 0) {
        return true;
    }
    const auto mask = 0xffU << (8 - remaining);
    return (a[whole] & mask) == (b[whole] & mask);
}

void differential() {
    std::mt19937 random(314159);
    for (const auto family : {IpVersion::v4, IpVersion::v6}) {
        Fib fib;
        std::vector<Route> reference;
        const std::size_t bits = family == IpVersion::v4 ? 32 : 128;
        for (unsigned iteration = 0; iteration < 4000; ++iteration) {
            IpAddress address{};
            for (auto& byte : address) {
                byte = static_cast<std::uint8_t>(random());
            }
            const auto length = random() % (bits + 1);
            const auto group = static_cast<std::uint32_t>(random());
            auto existing = reference.end();
            for (auto it = reference.begin(); it != reference.end(); ++it) {
                if (it->length == length && matches(address, it->prefix, length)) {
                    existing = it;
                    break;
                }
            }
            if (iteration % 3 == 0) {
                const bool found = existing != reference.end();
                check(fib.erase(family, address, length) == found, "reference erase disagreement");
                if (found) {
                    reference.erase(existing);
                }
            } else {
                check(fib.insert(family, address, length, group), "random insert");
                if (existing == reference.end()) {
                    reference.push_back({address, length, group});
                } else {
                    existing->group = group;
                }
            }
            check(fib.size() == reference.size(), "reference route count disagreement");
            // Exercise both the updated address and an unrelated destination.
            for (unsigned query = 0; query < 2; ++query) {
                if (query == 1) {
                    for (auto& byte : address) {
                        byte = static_cast<std::uint8_t>(random());
                    }
                }
                std::optional<FibMatch> expected;
                for (const auto& route : reference) {
                    if ((!expected || route.length > expected->prefix_length) &&
                        matches(address, route.prefix, route.length)) {
                        expected = FibMatch{route.group, route.length};
                    }
                }
                check(fib.lookup(family, address) == expected, "reference lookup disagreement");
            }
        }
    }
}

void copy_isolation() {
    Fib original;
    const IpAddress address{192, 0, 2, 1};
    check(original.insert(IpVersion::v4, {}, 0, 1), "copy default");
    check(original.insert(IpVersion::v4, address, 32, 2), "copy host");
    Fib copy(original);
    check(copy.erase(IpVersion::v4, address, 32), "copy erase");
    check(original.lookup(IpVersion::v4, address) == FibMatch{2, 32}, "copy changed original");
    check(copy.lookup(IpVersion::v4, address) == FibMatch{1, 0}, "copy fallback");
    check(original.insert(IpVersion::v4, {}, 0, 3), "mutate original");
    check(copy.lookup(IpVersion::v4, {}) == FibMatch{1, 0}, "original changed copy");
    copy = original;
    check(original.erase(IpVersion::v4, {}, 0), "erase shared root");
    check(copy.lookup(IpVersion::v4, {}) == FibMatch{3, 0}, "root alias after assignment");
}
} // namespace

int main() {
    boundaries();
    differential();
    copy_isolation();
    std::cout << "All FIB tests passed\n";
}
