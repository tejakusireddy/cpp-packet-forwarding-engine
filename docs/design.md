# Design decisions

This is a userspace forwarding library with a Linux reference executable. The
correctness model is implemented; the performance targets remain acceptance
criteria, not architectural guarantees. Read [the code guide](code-guide.md) for
the implementation walkthrough and [benchmarking](benchmarking.md) for evidence.

## 1. Separate packet interpretation from packet mutation

`parse_packet` borrows immutable bytes and validates structural bounds. It returns
a typed error or a view, without allocations. `forward_packet` checks eligibility
and route resolution before the first write. This gives callers a useful invariant:
every rejected frame is byte-for-byte unchanged. Tests exercise that invariant.

The parser accepts IPv4 options for structural inspection. The forwarding path
rejects them because options can require forwarding semantics beyond adjusting
TTL. It also rejects recognized IPv6 extension headers rather than treating those
bytes as a transport header. This bounds current behavior without pretending to
be a complete router. ICMP generation, local delivery, neighbor discovery, VLAN,
jumbograms, and address-policy checks remain outside this implementation.

## 2. One address representation and separate family roots

Network-order addresses are 16-byte arrays; IPv4 uses the first four bytes and
zeroes the tail. This connects parser and FIB without conversion or allocation.
Separate roots prevent a /0 IPv4 route from matching IPv6. Family-specific prefix
limits reject accidental /33 IPv4 and /129 IPv6 entries.

## 3. A binary trie with copy-on-write ownership

A bitwise trie makes longest-prefix matching easy to audit: remember the latest
route while walking destination bits. Lookup reads raw node pointers under table
ownership and allocates nothing. It takes at most 32 or 128 bit steps.

Snapshots share unchanged nodes. A writer copies a node only when more than one
owner exists, then descends through the private path. Fresh bulk construction
mutates unique nodes in place, avoiding a full path allocation per inserted route.
Deletion prunes only empty private nodes, preserving descendants and old versions.
The tradeoff is pointer chasing, shared ownership metadata, and binary-node memory
cost. A compressed/radix or multibit representation should replace it only after
like-for-like measurements establish a benefit. No million-route lookup speed is
inferred from algorithmic complexity alone.

## 4. Publish FIB and adjacencies as one version

A prefix lookup cannot safely use a new FIB and an old group table. A snapshot
owns both, exposes only const access, and validates group references during
construction. Publication changes the pair atomically. A worker acquires one
version per bounded batch; each completion reports that version.

Versions increase strictly. Incremental batches name their expected base version.
`publish_if_base` prevents a newer-numbered but stale-base batch from overwriting
an intervening update. Batches are all-or-nothing because unpublished candidate
state is discarded on validation/allocation failure.

Atomic shared ownership is a correctness baseline, not a lock-free reader claim.
Standard libraries can lock internally. A writer mutex serializes publication
and retirement; workers do not acquire that mutex. Replaced snapshots remain in
the store until no reader owns them. Control-thread collection avoids destroying
old routing trees in a forwarding call. Slow readers can retain memory, so memory
pressure and epoch-based reclamation remain future performance work.

## 5. Resolved adjacencies and deterministic equal-cost selection

A next hop already contains output interface and Ethernet addresses. ARP/ND is a
control-plane responsibility, so forwarding never blocks on neighbor discovery.
Unavailable members are filtered when a group is built, keeping selection O(1).
Group IDs and member IDs are opaque; zero is a valid value.

A fixed integer mixer maps the directional flow hash to an active-member index.
This is portable and inexpensive. Member order is meaningful; group changes can
remap flows. Weighted/resilient ECMP would require a different selection policy.

TCP/UDP ports enter the hash only when present in a non-fragmented datagram.
Every IPv4 fragment uses the same address/protocol key, including the first one.
This deliberately sacrifices some distribution to prevent fragment reordering.
Fragmented and unfragmented packets of a transport flow can use different paths.
The hash is non-cryptographic; it is not protection against adversarial flow keys.

## 6. SPSC queues and explicit backpressure

Each queue has exactly one producer and one consumer. This allows two cursors
and release/acquire publication without a contended general-purpose MPMC queue.
A multicore dispatcher owns a separate queue pair per worker.

Capacity is fixed. Failure to submit preserves ownership. An output-full worker
holds one completion and stops taking input; it never forwards that packet twice.
The control plane uses the same acknowledgment discipline. There is no silent
queue overflow policy. Callers choose whether to retry, stop reading, or count a
drop outside the queue. Indefinite downstream backpressure can delay shutdown;
there is no forced cancellation that discards accepted jobs.

## 7. Caller-owned threads and buffers

The library does not start hidden threads. Callers control scheduling, affinity,
thread creation, and joining. Buffer vectors transfer ownership through queues
and return in completions. A single-owner pool preallocates their storage and
recycles it without shared freelist contention. Buffer sizes and queue counts
are explicit. NIC DMA integration would need a different buffer representation.

Batches amortize snapshot reference counting. The budget is bounded, so one poll
does not monopolize the caller indefinitely. Routing updates become visible at
batch boundaries; this is a documented consistency choice.

## 8. Process IPC has a wire contract

Unix stream transport encodes explicit integer widths and byte order, never native
C++ object layouts. Framing and counts are bounded before processing. Reserved
fields must be zero so future extensions cannot be silently misinterpreted.
Protocol parsing happens on a transport/control thread, not a forwarding worker.

The transport handles interrupted/partial I/O with one absolute deadline across
prefix and payload. The optional runtime listener requires a private owner-only
directory and creates a mode-0600 socket. Filesystem ownership is the authorization
boundary; routing clients are trusted local processes under that account.

One transport thread handles one active client, with a four-connection backlog
and a 64-message session cap. A separate control thread performs snapshot work and
sleeps on a semaphore while no batch is queued.
This preserves the SPSC queue roles and bounds simultaneous buffering without a
thread-per-client design. Head-of-line blocking is an explicit tradeoff, bounded
by the message deadline and session cap. The service rejects group members that
refer to an output interface outside the reference runtime's single configured
egress. Packet workers perform no socket I/O or command parsing.

Shutdown cancels readiness polling, drains any accepted batch acknowledgment, and
joins transport/control threads. A disconnected sender may not know whether its
batch was published. Explicit versions allow detecting conflicts; the client does
not automatically retry a possibly applied update. Socket cleanup checks inode
identity and does not unlink existing paths at startup, avoiding takeover of an
unrelated file or another running service.

## 9. Linux packet sockets establish a reference path

AF_PACKET is readily auditable and works with veth integration tests. It involves
kernel socket I/O and per-packet syscalls; it is not a kernel-bypass backend.
The backend checks output MTU, zero-pads short frames, skips outgoing copies,
and reports nonblocking backpressure. The reference executable is unidirectional.

The dispatcher hashes flows to workers. It currently parses again in the worker;
that extra work is an explicit reference-runtime tradeoff. CPU affinity is opt-in
and fails visibly when requested CPUs are unavailable. A production NIC backend
should evaluate packet rings, receive-side steering, and batched I/O using measured
workloads rather than assuming the reference backend meets the target.

## 10. Keep performance claims tied to a measurement boundary

Correctness suites, sanitizer runs, in-memory benchmarks, and wire-level tests
answer different questions. Core-call latency excludes queues and NICs. Queued
latency includes scheduling and queue residence. Update rate currently excludes
process serialization and is measured separately from forwarding. Every saved
result identifies its mode, platform, revision, command, and sample count.

Linux builds and loopback/veth integration now pass in CI. The remaining gates are
Linux NIC benchmarks, simultaneous forwarding/update measurements, and queued/wire
p99 below 10µs on a documented eight-core setup. Passing correctness tests does
not close those performance gates.
