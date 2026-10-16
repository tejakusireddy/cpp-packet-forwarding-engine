# cpp-packet-forwarding-engine
High-performance userspace packet forwarding engine in C++20 with IPv4/IPv6, FIB longest-prefix lookup, ECMP, multicore processing, and asynchronous control/data-plane IPC.

The project is under development. The current implementation provides a
bounds-checked, allocation-free Ethernet II and IPv4/IPv6 parser and a mutable
IPv4/IPv6 longest-prefix FIB, resolved next-hop groups, and equal-cost multipath
selection. Packet transmission, multicore workers, and control-plane IPC are
planned. No throughput or latency claims have been measured yet.

## Build and test

Requires CMake 3.20+ and a C++20 compiler. Linux is the deployment target; the
portable parser can also be built on macOS.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

To check memory safety and undefined behavior with Clang or GCC:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

## Parser contract

`forwarding::parse_packet` accepts a borrowed byte span and returns a packet view
or a specific parse error. It reads network byte order without aligned pointer
casts. The view includes IP addresses, hop limit/TTL, protocol/next-header value,
and the complete IP datagram with Ethernet padding excluded. Keep the input
buffer alive for the lifetime of the view.

The parser checks IP versions, IPv4 header and total lengths, IPv6 payload length,
and buffer bounds. IPv4 options are accepted; IPv6 extension headers are retained
inside the datagram without traversal. VLAN tags and IPv6 jumbograms are not
supported. Checksums and transport headers are not validated. Parsing success
does not imply a packet is eligible for forwarding.

Tests cover both address families, truncation at every byte boundary, malformed
lengths and versions, padding, unaligned input, IPv4 options, empty IPv6 payloads,
and 20,000 deterministic randomized inputs. Linux CI runs GCC and Clang with and
without address/undefined behavior sanitizers.

## Forwarding information base

`forwarding::Fib` stores IPv4 and IPv6 prefixes in separate binary tries. Insert
adds or replaces an exact prefix; erase removes an exact prefix and prunes unused
nodes. Host bits are ignored. Prefix lengths span 0–32 for IPv4 and 0–128 for
IPv6, including independent default routes. IPv4 addresses use the first four
bytes of the same 16-byte address representation returned by the parser.

Lookup returns the longest matching prefix length and a next-hop group ID, or
an empty optional if no route matches. Group IDs refer to entries in the next-hop
table described below. Lookup performs no
allocations and traverses at most 32 or 128 address bits. Updates allocate nodes
as needed; this initial representation has not been validated at million-route
scale or against the throughput target.

The table supports concurrent readers only when there are no concurrent writes.
Snapshot publication and worker isolation will be implemented with control-plane
updates. Tests exercise every prefix length, replacement, deletion, family
isolation, and 8,000 deterministic randomized updates with 16,000 lookups checked
against an independent linear reference table.

## Next-hop resolution and ECMP

`forwarding::NextHopTable` maps group IDs to resolved Ethernet adjacencies:
member ID, output interface ID, source MAC, destination MAC, and availability.
The control plane must supply these values; ARP/neighbor discovery and interface
validation are not implemented. IDs are opaque and may be zero.

Group updates copy their input and replace the group in full. Empty groups and
duplicate member IDs are rejected without modifying the table. Unavailable
members are excluded during updates, leaving no per-packet member scan.
Groups with all members unavailable remain present and return an explicit error.

Resolution performs a longest-prefix lookup and selects an available member by
mixing a caller-supplied 64-bit flow hash and taking its remainder over the active
member count. The caller must use a stable hash for packets in the same flow;
packet-to-flow hashing is not implemented yet. Selection is deterministic for a
fixed hash and ordered active membership. Membership, ordering, or availability
changes may remap flows. Weighted and resilient ECMP are not implemented.

Results copy the matched prefix and selected adjacency without allocating.
Missing routes, missing groups, and groups without available members have distinct
errors. Resolution does not fall back to a less-specific route after finding a
route with an unusable group. Concurrent resolution requires both tables to remain
unchanged. This step does not decrement TTL/hop limit, rewrite frames, or transmit.

Tests cover parser-to-adjacency resolution for both address families, group
replacement and removal, failure paths, flow affinity, deterministic selection,
and distribution over 100,000 synthetic flow hashes.
