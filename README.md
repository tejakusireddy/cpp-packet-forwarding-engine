# cpp-packet-forwarding-engine
High-performance userspace packet forwarding engine in C++20 with IPv4/IPv6, FIB longest-prefix lookup, ECMP, multicore processing, and asynchronous control/data-plane IPC.

The project is under development. The current implementation provides a
bounds-checked, allocation-free Ethernet II and IPv4/IPv6 parser and a mutable
IPv4/IPv6 longest-prefix FIB, resolved next-hop groups, and equal-cost multipath
selection, in-place frame preparation, and queue-connected forwarding workers.
Packet transmission and control-plane IPC are planned. No throughput or latency
claims have been measured yet.

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

Run race detection in a separate Clang or GCC build:

```sh
cmake -S . -B build-thread -DCMAKE_BUILD_TYPE=Debug -DENABLE_THREAD_SANITIZER=ON
cmake --build build-thread --parallel
ctest --test-dir build-thread --output-on-failure
```

ThreadSanitizer and address/undefined behavior sanitizers cannot be enabled
together. Linux CI includes a separate ThreadSanitizer job.

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
member count. The caller must use a stable hash for packets in the same flow,
such as the packet-derived hash described below. Selection is deterministic for a
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

## Frame forwarding

`forwarding::forward_packet` parses a mutable Ethernet frame, validates the IPv4
header checksum, checks TTL/hop limit, and resolves its output adjacency. On
success it decrements TTL/hop limit, recomputes the IPv4 header checksum, and
rewrites destination and source Ethernet addresses. The result includes the
selected adjacency and frame length through the end of the IP datagram.
Payload, Ethernet type, and trailing padding remain unchanged. Any failure
returns a parse, resolution, or forwarding error without changing the frame.

This initial path handles IPv4 headers without options and IPv6 packets without
extension headers. IPv4 options and recognized IPv6 extension headers return an
explicit unsupported-options error for a future separate processing path.
IPv4 fragments can be prepared; callers must supply a consistent ECMP hash for
all fragments in a flow. No transport checksum update is needed for changes to
Ethernet addresses or TTL/hop limit.

The function allocates no memory and performs no transmission. The caller owns
the frame and may supply its flow hash or use the overload that derives it from
the parsed packet. A future transmit backend must ensure
minimum Ethernet padding and enforce output MTU. ICMP error generation, local
delivery, address-policy checks, and ARP/neighbor discovery are not implemented.
Tables must remain unchanged during the call.

Tests verify both address families for every forwarding TTL/hop limit (2–255),
IPv4 checksum correctness, exact mutation boundaries, preserved payload/padding,
all truncation boundaries, expired hops, corrupt checksums, unsupported headers,
and resolution failures. Rejected frames are checked for byte-for-byte equality.

## Packet flow hashing

`forwarding::flow_hash` accepts an unmodified view from `parse_packet` and hashes
the IP family, source address, destination address, and protocol/next-header value.
Non-fragmented TCP/UDP packets include the four port bytes when available within
the IP datagram. Other protocols and short transport payloads use addresses and
protocol only. This helper does not validate transport headers or checksums.

Every IPv4 fragment, including one with zero offset and the more-fragments flag,
uses addresses and protocol only. This keeps a fragmented datagram on one path;
fragmented and unfragmented traffic for the same transport flow may select
different paths. IPv6 extension headers are not traversed, and the forwarding
path continues to reject them. IPv4 option bytes are skipped when locating ports;
the forwarding path still rejects IPv4 options.

Hashing is directional, allocation-free, and deterministic across platforms. It
uses fixed byte-wise integer operations with an optional seed, excluding MACs,
TTL/hop limit, checksums, IPv6 flow labels, and payload. It is not cryptographic.
The three-argument `forward_packet` overload derives this hash without parsing a
second time. Tests cover flow fields, ignored fields, seeds, truncation around
ports, fragment affinity, option offsets, and automatic ECMP integration.

## Worker communication queue

`forwarding::SpscQueue<T, Capacity>` is a bounded, lock-free ring for exactly one
producer and one consumer. Each queue has `Capacity` usable slots and one spare
slot to distinguish full from empty. Storage is inline; the queue itself performs
no dynamic allocation. Element move/destructor operations must not throw, and
platforms without always-lock-free `size_t` atomics are rejected at compile time.
Element operations may have their own costs, so use lightweight descriptors for
worker communication.

`try_push` returns false when full without consuming the supplied value.
`try_pop` returns an empty optional when empty and transfers ownership otherwise.
Both operations return immediately without retry loops. Release/acquire ordering
publishes constructed elements and completed slot reuse. Producer and consumer
cursors occupy separate 64-byte-aligned storage. Move-only, non-default-constructible
elements are supported; copy insertion requires a non-throwing copy constructor.

Construct the queue before starting either thread and destroy it only after both
threads stop. Remaining elements are destroyed with the queue. Multiple producers
or consumers require separate queues or external synchronization. This queue is
for threads in one process; shared-memory/process IPC is not implemented.
Control-plane update messages are not connected yet.

Tests cover exact capacity, full/empty behavior, repeated wraparound with both
power-of-two and other capacities, move ownership, element destruction, and
750,000 ordered multiword messages passed between producer and consumer threads
across capacities 1, 3, and 1024. These are correctness tests, not throughput
benchmarks.

## Forwarding workers

`forwarding::ForwardingWorker<Capacity>` connects one input queue and one completion
queue to the forwarding path. A `PacketJob` owns a byte vector and caller-defined
ID. Successful submission transfers its buffer without copying; full or closed
input returns false and retains caller ownership. Each accepted job produces one
completion containing the same ID, owned frame, and forwarding result, including
for rejected packets. Error frames remain unchanged.

Each worker requires one submitting thread, one processing thread, and one
completion consumer. The submitter and completion consumer may be the same thread.
Call `poll` on the processing thread or call `run` to poll until closed and drained.
The caller creates and joins the processing thread. Multiple workers can read the
same FIB and next-hop table, which must remain unchanged and outlive all workers.
Dynamic routing-table publication is not implemented yet.

When the output queue fills, the worker retains one pending completion and stops
consuming input until that completion is published. It never forwards a pending
packet again. Storage is bounded to `Capacity` queued inputs, `Capacity` queued
outputs, and one in-flight/pending packet per worker, excluding caller-held jobs.
Buffer sizes are caller-controlled; this bounds packet count rather than bytes.
Queueing and forwarding do not allocate or copy packet buffers, though creating
jobs and threads can allocate.

The submitter calls `close` after its final submission. Continue receiving during
shutdown: a full output queue intentionally blocks further progress. `finished`
means every accepted job has been published; drain remaining completions and join
the processing thread before destroying the worker. `run` yields when idle or
backpressured. There is no forced cancellation or owned background thread.

Tests force capacity-one input/output backpressure, verify buffer ownership and
shutdown behavior, and pass 16,000 mixed IPv4/IPv6 jobs through eight worker threads
sharing immutable tables. Each completion is checked against direct forwarding,
including errors and per-worker ordering. This is a correctness test, not evidence
of eight-core throughput or latency targets. NIC I/O, CPU affinity, packet pools,
batching, and control-plane publication remain future work.
