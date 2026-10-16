# Benchmark protocol

Build with `CMAKE_BUILD_TYPE=Release`. Sanitizer timings are not performance
measurements. `forwarding_bench` prints JSON and returns failure if forwarding or
update acknowledgments fail. There are no performance thresholds in correctness
tests; a slow CI runner must not become a false correctness failure.

```sh
./build/forwarding_bench --mode core --workers 8 --packets 1000000 \
  --routes 1000000 --updates 100000 --batch 512
./build/forwarding_bench --mode queued --workers 8 --packets 1000000 \
  --routes 1000000 --updates 100000 --batch 512
```

`--packets` is per worker; `--routes` and `--updates` are totals. All routes in
this benchmark are distinct IPv4 /32 entries, with two installed next-hop groups.
Route generation uses sequential addresses in 10.0.0.0/8. Packet size is 64 bytes
including Ethernet and excluding FCS. There is no NIC or socket I/O in either
mode. Eight threads are not evidence of eight dedicated physical CPU cores.

## What the modes measure

| Mode | Throughput includes | Sampled latency includes | Excludes |
| --- | --- | --- | --- |
| `core` | Per-thread frame reset, parse, hash, lookup, resolve, rewrite, result check | One forwarding call | Dispatcher, queues, snapshot acquisition, NIC |
| `queued` | Dispatcher fill, pool movement, SPSC queues, batched snapshot acquisition, forwarding, completion consumption | Successful submission through completion receipt | Buffer preparation before submission, NIC |

The core loop uses 1,024 packet templates per worker; its active lookup working
set can fit in cache even when the FIB has a million entries. Queued mode walks
destinations modulo the entire configured route count. Do not compare the two
as equivalent workloads or use core mode as an end-to-end forwarding result.

A 10,000-packet warmup precedes timing. One packet in every 1,024 per worker is
sampled with `steady_clock`; p99 uses nearest rank on all collected samples.
Clock overhead is included in samples. The JSON reports sample count explicitly.
For useful tail statistics collect thousands of samples across multiple runs;
short smoke tests establish execution correctness only. Throughput uses wall
time, including thread synchronization and joining in core mode. Queued mode ends
when the dispatcher has received all completions, then joins drained workers.
There is no CPU pinning by default; record affinity and competing load separately.

`--window N` limits submitted-but-unreceived packets per worker in queued mode.
The default, 2,049, permits the full input/output/pending capacity. Smaller windows
bound queue residence at the cost of concurrency. Compare throughput and p99
together, and record the window with each result; a changed window is a changed
workload. Idle workers acquire no routing snapshot until they actually pop a job.

## Route and update measurements

Initial snapshot construction is timed separately. Update batches are prepared
before the update timer, then pass asynchronously through the control queue.
Each route update changes the selected group on successive passes through the
table. The timer includes snapshot path copying, group copying, publication,
acknowledgment backpressure, and control-thread scheduling. Retired snapshots are
collected during publication and once after the update timer.

The update rate counts individual route operations, not batches. IPC serialization
and process transport are excluded. Forwarding and update benchmarks run in
separate phases; their rates do not demonstrate simultaneous performance. Existing
concurrency tests establish correctness under publication, not throughput isolation.
Peak RSS is the process high-water mark across construction and both phases,
reported in KiB with OS-specific unit conversion.

## Recording reproducible evidence

Use the sweep runner to preserve all runs and environment metadata in a new directory:

```sh
python3 scripts/benchmark.py --output benchmarks/results/local
```

It runs both modes with 1, 2, 4, and 8 workers three times by default. It refuses
to overwrite an existing nonempty result directory and records dirty-tree state.
Unavailable CPU metadata is recorded as null rather than guessed.

For each run save the JSON, exact command, git revision, compiler version/build
flags, OS/kernel, CPU model, allowed CPU set, memory, and background load. Run 1,
2, 4, and 8 workers at least three times. Keep every run, including outliers and
errors. Compare the same mode, packet mix, route distribution, and batch size.
Use an external packet generator and hardware timestamps for wire-level claims.

The project targets 5M+ packets/s across eight cores, sub-10µs p99, one million
routes, and 100K+ route updates/s. A result supports only the workload and platform
actually measured. Linux NIC performance and simultaneous update/forwarding
performance require independent validation.

## Hosted Linux baseline

The `Hosted Linux benchmarks` workflow runs Release builds on `ubuntu-24.04` and
uploads all JSON records as `hosted-linux-benchmarks`. It repeats core and queued
measurements at 1/2/4/8 worker threads, then repeats queued measurements with an
in-flight window of 32. Each configuration has three runs, one million IPv4
routes, and 100,000 separately timed updates.

Environment metadata records the actual allowed CPU set, CPU topology/model,
virtualization, compiler/build flags, revision, and initial load average. Rows
explicitly flag worker counts exceeding the available CPUs. Eight threads on a
smaller VM cannot validate an eight-core target. Shared hosted runners are useful
for Linux baselines and regression experiments; they do not establish dedicated-
core, wire-level, or simultaneous forwarding/update performance.

The workflow can be dispatched after merge. The initial pre-merge run used a
temporary branch push trigger, removed after results were captured so ordinary
documentation pushes do not consume benchmark time. Summaries are generated by
`python3 scripts/summarize_benchmarks.py RESULTS_DIRECTORY`; raw repetitions are
retained for 30 days in the Actions artifact and can be saved in the repository.

The first [hosted Linux run](https://github.com/tejakusireddy/cpp-packet-forwarding-engine/actions/runs/38971551487)
is permanently saved under `benchmarks/results/hosted-linux`, including all 36
repetitions and both environment records. The VM exposed four vCPUs. Consult
the [validation report](validation.md) for the measured baseline and its limits.
