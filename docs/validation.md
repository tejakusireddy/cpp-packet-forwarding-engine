# Validation and remaining acceptance gates

## Local correctness

The portable build has thirteen component suites and two benchmark smoke tests. Release,
AddressSanitizer/UndefinedBehaviorSanitizer, and ThreadSanitizer runs pass on macOS
arm64 with Apple Clang 17. Tests cover packet bounds, reference-checked LPM, ECMP,
unchanged rejected frames, fragment hashing, SPSC publication, worker ownership,
snapshot concurrency/reclamation, transactional updates, and Unix process IPC.

## Linux CI correctness

[Pull-request run 38966703953](https://github.com/tejakusireddy/cpp-packet-forwarding-engine/actions/runs/38966703953)
passed all six jobs at revision `ad96b0fcfd5531c7eb819cfcf9804d4f479ec2dc` on
October 16, 2026. The matching push run also passed. GCC and Clang passed ordinary
and address/undefined-behavior builds; the separate ThreadSanitizer job passed.
All twelve portable CTest cases passed in each job. The thirteenth raw-socket case
was explicitly skipped without privileges and then passed in the dedicated
privileged loopback job. That job also passed IPv4/IPv6 forwarding through two
veth pairs in an isolated network namespace, checking MAC addresses, TTL/hop limit,
IPv4 checksum, payload, padding, and clean shutdown.

The initial Linux failure was a missing direct `<algorithm>` include in IPC tests.
The fix adds declaring headers explicitly rather than relying on macOS transitive
includes. All twelve public headers also compile independently with local Clang.
Linux correctness validation does not establish Linux NIC performance.

[Live-routing run 38970433708](https://github.com/tejakusireddy/cpp-packet-forwarding-engine/actions/runs/38970433708)
also passed every job for the listener/client/runtime integration. GCC/Clang,
sanitizer, and ThreadSanitizer jobs passed fifteen portable cases (plus the
unprivileged raw-socket skip). The privileged veth test kept one forwarder alive
while the client removed and restored its IPv4 default, installed a new adjacency
and host route, rejected stale/missing-group/wrong-egress requests, and verified
unchanged IPv6 forwarding. Shutdown with an idle routing client removed the socket.
These demonstrate live-update correctness rather than sustained update throughput.

## Saved benchmark sweep

[Raw results and environment metadata](../benchmarks/results/macos-arm64/environment.json)
were collected at revision `df3064eba2b050c660f104dc2b65f3723959eda7` with a clean
working tree. Platform: Darwin 24.6.0 arm64, 11 reported logical CPUs, Apple Clang
17, Release `-O3 -DNDEBUG`. CPU model/allowed-set queries were unavailable; affinity
and background load were uncontrolled. These are local in-memory results.

Each mode/worker count has three saved runs, one million packets per worker,
one million distinct IPv4 /32 routes, 100,000 route updates, and batches of 512.
The table shows medians of per-run metrics, not a pooled p99:

| Mode | Workers | Packets/s | Sampled p99 | Route updates/s |
| --- | ---: | ---: | ---: | ---: |
| Core | 1 | 15.71M | 0.167µs | 4.19M |
| Core | 2 | 31.69M | 0.167µs | 4.25M |
| Core | 4 | 54.20M | 0.167µs | 4.46M |
| Core | 8 | 84.75M | 0.209µs | 4.18M |
| Queued | 1 | 5.31M | 447.917µs | 4.50M |
| Queued | 2 | 8.82M | 673.500µs | 4.47M |
| Queued | 4 | 11.43M | 662.042µs | 4.46M |
| Queued | 8 | 11.42M | 1,018.167µs | 4.24M |

All saved runs report zero forwarding errors. The route-update phase is separate
from forwarding and excludes process IPC. Core mode's active lookup set is small;
queued mode walks the table. The throughput difference is not just queue cost.
Read [measurement boundaries](benchmarking.md) before comparing either mode with
the project target. Raw repetitions remain committed so variance is visible.

## Hosted Linux baseline

[Run 38971551487](https://github.com/tejakusireddy/cpp-packet-forwarding-engine/actions/runs/38971551487)
measured the merged forwarding baseline at revision
`ef761f15a1d0b5238796cc2c87c210b46b4f9d86`. The runner exposed four x86-64 vCPUs
on a Microsoft-virtualized AMD EPYC 9V74 system, Linux `6.17.0-1022-azure`, GCC
13.3, and Release `-O3 -DNDEBUG`. Its reported topology is virtual; the host CPU
model does not grant access to all of that processor's physical cores. There was
no CPU pinning or control over neighboring host load.

All [36 raw repetitions and environment records](../benchmarks/results/hosted-linux/provenance.json)
are saved. Each run built one million distinct IPv4 /32 routes, forwarded one
million packets per worker, and separately applied 100,000 route operations in
batches of 512. All runs reported zero forwarding errors. These median per-run
results use the smaller queue window of 32:

| Workers | Allocated vCPUs | Queued packets/s | Sampled p99 | Separate route updates/s |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 4 | 4.26M | 9.925µs | 4.40M |
| 2 | 4 | 9.17M | 10.906µs | 4.34M |
| 4 | 4 | 9.34M | 33.139µs | 4.42M |
| 8 | 4 | 11.03M | 119.697µs | 4.39M |

The eight-worker row oversubscribes the VM. It is not an eight-core measurement.
At the default 2,049-packet window, four workers measured 8.51M packets/s with
3,000.639µs sampled p99, demonstrating why queue depth must accompany latency
claims. The smaller window changes the offered workload and queue residence.
The sub-10µs project target is not established by a single-worker median near
10µs; it remains unverified for the required eight-core forwarding workload.

Core mode with four workers measured 27.96M calls/s and 0.211µs sampled p99, but
excludes queues and uses a small active lookup set. No hosted measurement includes
NIC I/O, process IPC costs, or simultaneous updates/forwarding. Future target runs
need a controlled Linux host with enough dedicated processing capacity for workers,
dispatcher, and control activity, plus an explicit packet-path timing boundary.

## Target status

| Target | Evidence | Status |
| --- | --- | --- |
| Ethernet/IPv4/IPv6 parsing and forwarding preparation | Portable correctness/sanitizer tests | Implemented within documented header limits |
| Longest-prefix lookup, next-hop resolution, ECMP | Reference and integration tests | Implemented |
| Multicore worker queues | Eight-thread ordering/ownership tests | Implemented |
| Versioned batched asynchronous updates and IPC | Concurrent tests, forked-process exchange, live-service tests | Implemented library and optional runtime listener |
| 1M+ routes | One million distinct IPv4 routes in saved runs | Demonstrated locally for that route distribution |
| 100K+ updates/s | Async control queue, incremental snapshots, separate update phase | Exceeded locally; Linux/process-inclusive rate unverified |
| 5M+ packets/s across eight cores | Eight worker threads exceed rate in queued macOS benchmark | Linux eight-dedicated-core/wire claim unverified |
| Sub-10µs p99 forwarding latency | Core calls satisfy it; queued path exceeds it | Not met for queued/end-to-end path |
| Linux receive/transmit correctness | Privileged loopback and dual-stack veth CI jobs passed | Verified for reference integration workload |
| Forwarding isolation under sustained updates | Concurrency correctness tests | Simultaneous performance measurement pending |

## Next performance experiments

The benchmark now supports a per-worker in-flight window, and workers avoid
snapshot acquisition during empty polls. Measure active-set size, random
prefix distributions, IPv6 route scale, idle scheduling, affinity, and IPC costs
separately. Repeat on a documented Linux machine and include the full packet path
before publishing a full-path performance claim. The current queued tail-latency gap
must remain visible even when core microbenchmarks are much faster.
