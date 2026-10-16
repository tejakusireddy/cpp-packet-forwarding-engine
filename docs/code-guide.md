# Reading the implementation

This guide follows the code in dependency order. It explains each component's
functions, state, important branches, and ownership decisions. Header comments
define public contracts; [design decisions](design.md) explain the tradeoffs.
Tests are executable examples for the boundary cases. Read them alongside the
corresponding implementation rather than interpreting every check as boilerplate.

## 1. Build configuration: `CMakeLists.txt`

`forwarding` is the reusable C++20 library. Public include paths, language level,
thread linkage, and sanitizer flags propagate to executables that use it.
Warnings are errors for project sources. Compiler extensions are disabled for
the library and portable executables. Linux-only source is selected by the target
system name, keeping Linux socket headers out of the macOS build.

CTest registers each independent component suite. Concurrency tests have timeouts
so a broken shutdown or ring protocol fails rather than hanging CI indefinitely.
ThreadSanitizer uses a separate build because it cannot run together with address
sanitization. Benchmark smoke tests assert successful execution, not machine-
dependent performance thresholds. `BUILD_BENCHMARKS=OFF` omits the benchmark binary;
`BUILD_TESTING=OFF` builds just the library and enabled tools.

`.github/workflows/build.yml` covers Linux GCC/Clang, address/undefined-behavior
sanitizers, race detection, and privileged packet I/O in an isolated namespace.
Configured jobs are not evidence that they have run for an unpushed revision.

## 2. Wire bytes: `packet.hpp` and `packet.cpp`

`PacketView` holds version, addresses, hop limit, protocol, and a borrowed span of
the complete IP datagram. `ParseResult` is a variant so callers must distinguish
success from an explicit structural error. No object owns a copy of packet data.

`read_u16` assembles two bytes with shifts. This handles network byte order without
assuming alignment or violating C++ pointer-aliasing rules. It is used only after
the caller has established the corresponding minimum length.

`parse_packet` first requires the 14-byte Ethernet II header. Bytes 12–13 select
IPv4 (`0x0800`) or IPv6 (`0x86dd`); unsupported EtherTypes return early. The IP span
begins at byte 14. Version checks prevent mismatched EtherType/header combinations.

For IPv4, the low nibble of byte zero gives IHL in four-byte words. IHL must be at
least 20 bytes, fit in the input, and not exceed the declared total length. Total
length must fit in the input. Source/destination start at IP offsets 12/16, TTL at
8, and protocol at 9. Only four address bytes are copied; value initialization
leaves the IPv4 address tails zero.

For IPv6, the fixed header is 40 bytes and payload length is read at offsets 4–5.
Next header is offset 6, hop limit 7, and addresses 8/24. A zero payload with a
Hop-by-Hop next header is conservatively rejected as an unsupported jumbogram
case. Other empty payloads can be represented. The returned datagram ends at the
declared IP length, so Ethernet padding never becomes transport data.

`packet_tests.cpp` checks every truncation point, unaligned input, versions,
length relations, option lengths, empty payloads, padding, and randomized bounds.
Parsing does not prove an IPv4 checksum or a transport packet is valid.

## 3. Prefix lookup: `fib.hpp` and `fib.cpp`

`FibMatch` stores the matched length and next-hop group. Optional storage permits
group ID zero; a missing optional is different from a zero value.

`address_bits` centralizes family validation. `bit_at` reads most-significant bits
first, which matches network prefix notation. Each node owns two shared children
and an optional route. The two root pointers separate IPv4 and IPv6 defaults.

`writable` creates an absent node, copies a shared node, or returns a unique node
unchanged. Copying the node also shares its children. Descending repeats this
decision, so only touched shared paths are copied. `use_count` is not general
synchronization: callers still must not mutate the same `Fib` concurrently with
reads or writes. Independent copies share immutable old nodes safely.

`insert` validates before mutation, follows exactly the prefix length, and creates
private nodes as needed. Host bits are never examined. It increments size only
when the final node did not already contain a route, then upserts the group ID.
An allocation failure may leave unused path nodes in a mutable candidate but
does not install a partial route. Snapshot construction discards failed candidates.

`erase` first checks that the exact route exists, avoiding copy allocations for a
miss. It makes the path private, saves links in a fixed 128-entry array, clears
the route, decrements size, and prunes empty child nodes from leaf to root. It
stops at any remaining route or child. Copying can allocate, so erase can throw.

`lookup` walks raw pointers while the owning table remains alive. It updates
`best` whenever a route is found, including at depth zero, then follows the next
destination bit until a missing child or family limit. It never increments node
reference counts during traversal and never allocates.

`fib_tests.cpp` covers all prefix lengths, overlap/fallback, defaults, group zero,
copy isolation, pruning, and randomized comparison with a byte-mask reference.

## 4. Adjacencies: `next_hop.hpp` and `next_hop.cpp`

`NextHop` is already resolved: member ID, output interface, source/destination MAC,
availability. `ResolvedRoute` combines it with the FIB match. Results own values,
so they survive table replacement.

`upsert` validates an entire group into temporary state. An ID set detects duplicate
members; unavailable members are omitted from the active vector. Only after all
checks pass does `insert_or_assign` replace the group. An all-unavailable group
is valid metadata with an empty active vector. Empty input groups are rejected.

`mix` uses fixed unsigned 64-bit arithmetic. Overflow is intentional and defined
modulo 2^64. `resolve` looks up the route, finds its group, rejects missing/empty
active sets distinctly, then selects `mix(hash) % active_count`. There is no scan
for available members and no fallback to a less-specific route after a match.
`erase`, `contains`, and `size` support control-plane table management.

`next_hop_tests.cpp` checks replacement rollback, owned results, error categories,
deterministic mapping, availability, distribution, and parser-to-adjacency flow.

## 5. Flow identity: `flow.hpp` and `flow.cpp`

`flow_hash` consumes an unmodified parsed view. It uses fixed FNV-style byte-wise
operations, an optional seed, a family byte, source address, destination address,
and protocol. Explicit byte order avoids platform-dependent `std::hash` behavior.
IPv4 uses four address bytes; IPv6 uses sixteen.

IPv4 flags/offset are at offsets 6–7. Mask `0x3f` tests MF plus the high fragment
offset bits; the following byte contains the low offset bits. DF is deliberately
excluded. Fragmented packets skip ports even at offset zero. For ordinary TCP/UDP,
four port bytes are appended only when they fit after the IPv4 IHL or IPv6 fixed
header. Other protocols and short payloads use the address/protocol key. Extension
headers are not traversed. MACs, TTL, checksums, payload, and IPv6 flow labels do
not enter the key.

`flow_tests.cpp` contains a fixed expected hash, field-sensitivity tests, ignored-
field tests, port-boundary tests, fragment affinity, option offsets, and integration
with the automatic forwarding overload.

## 6. Packet preparation: `forward.hpp` and `forward.cpp`

`ForwardResult` separates parse, resolution, and forwarding errors. On success it
returns a copied adjacency plus frame length through the IP datagram.

`checksum` sums network-order 16-bit words and folds carries before complementing.
`extension_header` identifies IPv6 headers requiring the unsupported processing
path. `forward_impl` parses, rejects unsupported options, validates IPv4 checksum,
rejects hop limits zero/one, chooses the supplied or derived flow hash, and resolves
the route. Every one of those exits occurs before a write.

After successful resolution, the function decrements TTL/hop limit. IPv4 checksum
bytes are cleared and recomputed over the supported 20-byte header. Destination
MAC replaces Ethernet bytes 0–5 and source MAC replaces 6–11. EtherType, payload,
addresses, and padding remain untouched. TTL/hop limit is not part of transport
pseudo-headers, so no TCP/UDP checksum update is required. The two public overloads
share this implementation and parse only once.

`forward_tests.cpp` checks all supported hop values, output fields, checksum,
preserved bytes, and every rejection path without mutation.

## 7. Queue ownership: `spsc_queue.hpp`

This template is entirely in its header so each element/capacity specialization
can be compiled. It requires lock-free cursor atomics and non-throwing element
move construction/destruction. Optional slots avoid default-constructing `T` and
make element lifetime explicit. Capacity plus one reserves the full/empty marker.

The producer alone changes its cursor. It reads its own cursor relaxed, reads
the consumer cursor acquire, and returns false if the next slot would overlap.
Only after constructing the element does it store its cursor release. A failed
push performs no move from the caller's object.

The consumer reads its own cursor relaxed and the producer cursor acquire. It
moves out a published element, destroys the moved-from slot, then stores its
cursor release. That store pairs with the producer's acquire before slot reuse.
The two release/acquire pairs protect both construction visibility and destruction
before overwrite. Separate 64-byte-aligned cursors reduce false sharing.

There is no retry loop inside queue operations. A stale peer cursor can cause a
conservative full/empty return, which callers retry. Destruction requires stopped
threads. The queue does not enforce the one-producer/one-consumer rule dynamically.
`queue_tests.cpp` verifies ownership, lifetimes, wraparound, and multiword message
publication between threads.

## 8. Buffers and workers: `packet_pool.hpp` and `worker.hpp`

`PacketPool` allocates its vector stack and frame storage at construction. `acquire`
moves a free vector out and resizes within reserved capacity. `recycle` checks
capacity/free-slot limits, clears logical length, and moves it back. These calls
belong to one dispatcher thread. Workers receive the vector, not a pool reference.

`PacketJob` owns an ID and frame. `PacketCompletion` returns both with a result
and routing version. `ForwardingWorker` has one input ring, one output ring,
one optional pending completion, and atomic close/finished flags. It uses either
static table pointers or a snapshot store pointer; constructors establish which.

`try_submit` checks close and delegates ownership transfer to the input ring.
`close` is called by that same submitting thread after its last job. `try_receive`
belongs to the output consumer. `finished` means publication is done, not that
the consumer has drained every completion.

`poll_batch` calls `poll_one` up to its budget. The first newly popped packet
acquires the batch snapshot; idle and pending-only polls avoid shared-pointer
loads entirely. `poll`
uses budget one. `poll_one` retries a pending completion before reading another
job. Otherwise it loads close with acquire *before* checking the input ring: if
close is observed, preceding producer submissions must be visible before declaring
the input drained. A popped job is forwarded once and converted to pending output.
Output-full leaves that object intact for the next poll. `run` batches until stopped
and yields on idle/backpressure. The caller drains outputs and joins the thread.

`worker_tests.cpp` forces backpressure, tracks actual buffer addresses, verifies
shutdown and rejected results, exercises pooled batches, and compares eight workers'
outputs against direct calls. None of these tests measures CPU-core throughput.

## 9. Routing versions: `snapshot.hpp` and `snapshot.cpp`

The full constructor builds groups before routes so every reference can be checked.
Duplicate group IDs are invalid; repeated route prefixes upsert in order. The
incremental constructor shares the base FIB, copies group metadata, applies group
upserts, then ordered route changes. Missing deletes are idempotent. It requires a
newer version and exposes no partially constructed state.

`SnapshotStore` begins at an empty version zero. `acquire` uses an atomic shared-
pointer load. Feature detection selects the C++20 specialization or compatible
free functions. Readers retain a complete immutable version.

`publish` serializes writers, collects unused retired versions, checks monotonic
version, retains the old version, and atomically replaces current. `publish_if_base`
adds an exact expected-base check and one strong compare/exchange. Keeping an old
reference before exchange prevents a worker from destroying the last owner during
publication. `collect_retired` removes snapshots owned only by the retired list;
their destructors run on that calling control thread.

Tests check invalid construction, copies of input data, retained readers, version
limits, concurrent publishers/readers, incremental isolation, and completion-version
stability under backpressure. Reader synchronization still includes shared-pointer
reference counting and is not guaranteed lock-free.

## 10. Update processing: `control.hpp`

`UpdateBatch` owns expected/new versions, route changes, and group upserts.
`UpdateAck` reports requested version, current version at acknowledgment creation,
and a typed status. Another publisher may advance current after an applied update;
the two version fields are intentionally separate.

`ControlPlane` mirrors worker queue/close/pending state. `poll` acquires a base,
rejects a stale expected version, builds a candidate, and calls `publish_if_base`.
Invalid arguments and allocation failures become status values. The entire candidate
is discarded on failure. An output-full acknowledgment is held without rebuilding
or publishing again. All snapshot construction occurs on the control thread.
`control_tests.cpp` checks rollback, stale-base conflicts, ordered asynchronous acks,
and acknowledgment backpressure.

## 11. Process boundary: `ipc.hpp` and `ipc.cpp`

`Writer::put` appends explicitly sized big-endian integers and enforces the byte
limit. `Reader::get` bounds-checks before advancing its cursor; `zero` validates
reserved fields. Header helpers validate magic and protocol version.

`encode` serializes batches. `decode` bounds counts, validates field ranges, and
requires complete consumption. It does not validate routing references: the
control plane does that transactionally. [The protocol tables](ipc-protocol.md)
give field offsets and sizes.

`transfer` loops over partial reads/writes and EINTR, converts errors and EOF to
exceptions, and suppresses SIGPIPE on supported Linux/macOS socket APIs. Framing
helpers send/read a four-byte length with a bounded payload. Batch and ack wrappers
connect wire types to application types. After partial I/O failure, callers close
the stream; retrying on a desynchronized connection is unsafe.

`ipc_tests.cpp` checks round trips, every truncation point, malformed fields,
randomized input, disconnected peers, SIGPIPE handling, and a forked routing process
that submits a batch and receives an applied acknowledgment.

### Listener and live service

`socket_wait.hpp` computes a steady-clock deadline and polls readiness in intervals
of at most 20ms. It checks cancellation and the absolute deadline before each wait.
Prefix and payload transfers share that deadline, so a client cannot extend the
operation indefinitely by trickling bytes. `MSG_DONTWAIT` keeps each syscall
nonblocking after readiness. Deadline/cancellation errors invalidate the stream.

`unix_socket.cpp` owns descriptors with `UnixConnection`. Client connect checks
completion with `SO_ERROR` after readiness. `UnixListener` validates a private
parent, binds without deleting existing entries, sets socket permissions, listens
with backlog four, and records the filesystem device/inode. Nonblocking accept
returns an owned connection or no work. Destruction unlinks only the recorded
socket, preserving any file that replaced that path.

`RoutingService` owns one transport thread and one control thread. Construction
binds before starting threads; partial startup failure closes/joins the control
thread. A binary semaphore wakes the control thread for one submitted batch or
shutdown; it blocks while idle instead of consuming a CPU. Exactly one request
is outstanding, so each wakeup maps to one poll and acknowledgment. `serve`
accepts one client and processes at most 64 requests before closing
the session. It checks the configured egress policy, submits an allowed batch,
consumes exactly its acknowledgment, and sends the response. Once accepted by the
control queue, a batch is drained even during shutdown. A malformed or timed-out
connection closes independently; the listener continues accepting other clients.
`stop` requests transport cancellation and joins transport before control.

`routing_service_tests.cpp` checks persistent sessions, actual worker version/MAC
changes, invalid/stale rejection, egress policy, malformed peers, timeout recovery,
idle-client shutdown, and socket cleanup. `unix_socket_tests.cpp` covers endpoint
ownership, path collisions, private permissions, deadlines, and cancellation.

`parse_routing_request` parses strict unsigned version/group/interface values,
IPv4/IPv6 CIDRs with `inet_pton`, MAC pairs, and 0/1 availability. No socket opens
until parsing succeeds. `route_ctl` connects, sends one batch, verifies the ack's
version, and prints JSON with a typed status. Its exit code distinguishes applied,
rejected, malformed command, and transport failure. `routing_request_tests.cpp`
checks malformed fields, address-family limits, and full-width 64-bit versions.

## 12. Linux reference path: `linux_io.cpp` and `main.cpp`

`PacketSocket` validates an interface name, resolves its index, opens a nonblocking
close-on-exec raw socket, reads MTU/MAC, and binds. Constructor failure closes the
descriptor; the destructor closes a successful socket. `receive` uses `MSG_TRUNC`
to detect insufficient caller capacity, ignores outgoing copies, and treats
EAGAIN as no packet. `transmit` checks header/MTU limits, uses zeroed stack padding
for short frames, selects the bound output interface, and returns false for
temporary socket pressure. `pin_current_thread` makes requested affinity explicit.

The executable parses interface/MAC/worker/count/CPU arguments, installs two default
routes, optionally starts `RoutingService` with `--control`, starts workers, and
preallocates buffers. A dispatcher receives one pending
input, hashes it to a worker, consumes completions, retries blocked transmissions,
and recycles buffers. Output MTU violations and forwarding errors count as drops.
Normal signals stop reception, cancel routing sessions, drain accepted control
batches, then drain accepted packet work. `Workers` also closes,
drains, and joins threads during exception cleanup so joinable destructors do not
terminate the program. Exceptional cleanup may discard completions because normal
transmission is no longer possible.

`linux_io_tests.cpp` checks raw loopback and padding. `linux_integration.sh` creates
an isolated namespace; `linux_forwarding.py` sends valid IPv4 and IPv6 packets
through veth links and checks the output. Its live mode invokes `route_ctl` while
the same runtime stays active: erase/reinsert a default, add a new group and host
route, verify the changed Ethernet destination, reject stale/invalid requests, and
shutdown with an idle routing client. These require Linux privileges and are
not exercised by portable macOS CTest runs.

## 13. Evidence: `benchmarks/forwarding_bench.cpp` and `scripts/benchmark.py`

`options` parses bounded positive numbers; `address` generates distinct IPv4 hosts;
`build` creates a million-route-capable workload. `fill` builds valid 64-byte frames.
`core` uses per-thread template arrays and direct forwarding. `queued` uses real
worker queues and a dispatcher pool, with sampled submission timestamps stored in
a ring larger than maximum in-flight jobs per worker. `updates` prebuilds batches,
runs the asynchronous control plane, and validates applied acknowledgments.

`main` warms the code path, executes one forwarding mode, measures updates, computes
nearest-rank sampled p99, and emits JSON plus process peak RSS. The sweep script
records revision, dirty state, build cache, OS/compiler metadata, commands, and
every repetition without overwriting previous evidence. [Benchmark boundaries](benchmarking.md)
explain exactly which costs and workloads each number includes.
