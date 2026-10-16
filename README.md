# cpp-packet-forwarding-engine
High-performance userspace packet forwarding engine in C++20 with IPv4/IPv6, FIB longest-prefix lookup, ECMP, multicore processing, and asynchronous control/data-plane IPC.

The project is under development. The current implementation provides a
bounds-checked, allocation-free Ethernet II and IPv4/IPv6 parser and a mutable
IPv4/IPv6 longest-prefix FIB, resolved next-hop groups, and equal-cost multipath
selection, in-place frame preparation, queue-connected forwarding workers, and
versioned routing snapshots. Packet transmission and control-plane IPC are
planned. No throughput or latency
claims have been measured yet.

The benchmark executable now produces measured JSON for in-memory core and queued
workloads. Follow [the benchmark protocol](docs/benchmarking.md) before interpreting
those results; Linux wire-level targets remain unverified.

On Linux, an AF_PACKET reference backend supplies nonblocking receive/transmit,
MTU checks, minimum Ethernet padding, and optional worker affinity. It requires
`CAP_NET_RAW`; the portable build excludes its Linux-specific source.

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
Routing snapshots provide immutable table ownership and atomic publication.
Tests exercise every prefix length, replacement, deletion, family
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
Control-plane batches use a separate queue pair described below.

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
Alternatively, construct workers with a `SnapshotStore` for concurrent publication
as described below.

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
batching, and control-plane update transport remain future work.

## Versioned routing snapshots

`forwarding::RoutingSnapshot` builds and owns a complete FIB and next-hop table
from route and group entries. Its public API exposes only const access. It copies
input data, rejects invalid prefixes, missing group references, empty groups,
duplicate group IDs, and duplicate member IDs. Duplicate route prefixes use the
last entry. A group whose members are all unavailable is valid and yields the
normal no-available-members resolution error. Failed construction cannot modify
the published state.

`forwarding::SnapshotStore` starts with empty version zero. `publish` atomically
replaces both tables only with a strictly newer version; null, stale, and duplicate
versions are rejected. Concurrent publishers cannot regress the current version.
Versions are unsigned 64-bit values and do not wrap; reaching the maximum requires
a new store to start another version sequence.

Readers acquire shared ownership of one immutable snapshot. Old snapshots remain
valid until the final reader releases them. Workers constructed with a store
acquire once per bounded polling batch and report that version in `PacketCompletion`.
Queued packets use the snapshot current at processing time. Pending completions
retain their original result and version across subsequent publications. Static
table workers report version zero. Keep the store alive until its workers stop.

Publication uses standard atomic operations on shared pointers, including the
portable free-function interface supported by the macOS standard library. These
operations are not guaranteed lock-free. Writers are serialized with a mutex;
workers never take that writer mutex. The store retains replaced snapshots until
`collect_retired` finds no readers, then destroys them on the calling control
thread. Publication also collects unused retired snapshots. Call collection after
readers finish when updates cease; otherwise those retired versions remain until
store destruction. Readers holding old versions can increase retained memory.
Per-batch shared reference counting still requires performance work before
claiming isolation at the target throughput or latency.

Incremental snapshots share unchanged FIB nodes and copy a trie path only when
an update touches a shared node. A fresh table still mutates uniquely owned nodes
in place. Group metadata is copied per snapshot. Route changes are ordered;
deletion of a missing prefix is idempotent. Group deletion uses a full replacement
snapshot. Asynchronous update transport and large-table scaling remain to be
validated. Tests cover validation, retained-reader lifetime, version boundaries,
two concurrent publishers with four readers, and worker forwarding during 200
publications. Another test verifies that backpressured completions keep their
processing version while newer snapshots are published.

## Asynchronous control plane

`ControlPlane<Capacity>` accepts owned `UpdateBatch` objects through an SPSC queue
and returns ordered acknowledgments through another. A batch carries its expected
base version, new version, ordered route inserts/deletes, and group upserts.
One control thread validates and builds an incremental snapshot, then publishes
only if the expected base is still current. This prevents a stale batch from
overwriting an intervening publisher even when its proposed version is higher.
Validation or allocation failures leave the current snapshot unchanged.

Acknowledgments distinguish applied, stale-base, invalid, and resource-exhausted
batches. Output backpressure holds one pending acknowledgment without applying
the batch again. Close, receive, drain, and join follow the worker lifecycle.
This queue connects threads in one process; the Unix stream adapter below allows
an external routing process to exchange batches. There is no update-rate
performance claim yet.

## Batching and packet buffers

`ForwardingWorker::poll_batch(budget)` processes at most the supplied number of
completions and acquires one snapshot for the batch. `poll()` retains its one-step
behavior; `run()` uses a budget of 32. Backpressure stops the batch immediately.
This amortizes snapshot ownership without unbounded processing or changing FIFO
order. A routing update may become visible at the next batch boundary.

`PacketPool` preallocates a fixed number of vectors with a configured frame
capacity. A dispatcher acquires one, fills it, submits it, consumes the returned
completion, and recycles its vector. Buffers move through queues without copying.
Acquire/recycle do not grow their storage after construction; acquiring a size
larger than capacity or exhausting the pool returns an empty optional. The pool
is single-owner; workers never access it directly. Newly exposed vector bytes are
zero-initialized. Packet count is bounded, and maximum frame capacity is explicit.

## Process IPC

`ipc::send_batch` and `receive_batch` exchange a version-one binary protocol over
a caller-owned Unix stream socket. `send_ack` and `receive_ack` return control-plane
status. A transport thread receives a batch, submits it to `ControlPlane`, waits
for its acknowledgment, and sends the acknowledgment back. Calls handle short
reads/writes and interruptions. The application owns connection establishment,
timeouts, peer authorization, and reconnect policy; close a connection after any
framing or I/O error. Transport calls block and never belong on forwarding threads.

Messages have a four-byte length followed by network-byte-order fields. Payloads
are limited to 1 MiB, route changes to 32,768, groups to 1,024, and members per
group to 256. Decoders reject unsupported protocol versions, invalid enums,
nonzero reserved fields, truncated messages, and trailing data before publication.
Counts are bounded before iterating. No native C++ struct is sent over the wire.
The IPC test forks a routing process and exchanges a batch and applied ack over
a Unix socketpair, alongside codec and truncation tests.

## Linux packet I/O

`PacketSocket(interface)` binds a raw nonblocking socket to one Linux interface.
`receive` writes into caller storage, skips outgoing copies, and reports truncated
input instead of forwarding a partial frame. `transmit` enforces output MTU and
zero-pads short Ethernet frames to 60 bytes, excluding device-generated FCS.
Backpressure returns false; the application must retain and retry the frame.
The socket is closed by RAII. This syscall-per-packet backend establishes a
reference path; it is not a claim of kernel bypass or line-rate performance.

`pin_current_thread(cpu)` optionally pins a Linux thread to an allowed CPU ID.
Pinning to an unavailable CPU is an explicit error. No affinity is imposed on
the user's machine by default. Linux CI runs the raw-socket loopback integration
test with privileges, checking receive/transmit, padding, and output MTU rejection.
Ordinary unprivileged CTest runs skip that test when `CAP_NET_RAW` is unavailable.

## Benchmarks

```sh
./build/forwarding_bench --mode queued --workers 8 --packets 1000000 \
  --routes 1000000 --updates 100000 --batch 512
```

The JSON separates FIB construction, forwarding rate, sampled p99 latency, update
rate, and peak RSS. `core` measures direct calls with a small active working set;
`queued` includes dispatcher/worker queues and walks the configured route table.
Neither includes NIC I/O. Read [measurement boundaries and reproduction steps](docs/benchmarking.md).
