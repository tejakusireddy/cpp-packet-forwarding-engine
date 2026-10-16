"""Summarize raw repetitions without treating worker count as CPU allocation."""
import collections
import json
import pathlib
import statistics
import sys


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: summarize_benchmarks.py RESULTS_DIRECTORY")
    groups = collections.defaultdict(list)
    for path in pathlib.Path(sys.argv[1]).rglob("*.json"):
        result = json.loads(path.read_text())
        if "mode" not in result:
            continue
        if result["errors"] != 0:
            raise SystemExit(f"Forwarding errors in {path}")
        key = (result["mode"], result["workers"], str(result.get("inflight_window", "unspecified")))
        groups[key].append(result)
    if not groups:
        raise SystemExit("No benchmark runs found")
    print("## Hosted Linux in-memory baseline\n")
    print("Neither mode includes NIC I/O. Updates run in a separate phase, excluding process IPC.")
    print("Worker threads exceeding allocated CPUs are oversubscribed; they are not dedicated-core measurements.\n")
    print("| Mode | Workers | Window | CPUs available | Runs | Median Mpps | Median per-run p99 (µs) | Median route updates/s |")
    print("| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |")
    for (mode, workers, window), rows in sorted(groups.items()):
        cpus = ",".join(sorted({str(row.get("available_cpus", "unknown")) for row in rows}))
        rate = statistics.median(row["packets_per_second"] for row in rows) / 1000000
        p99 = statistics.median(row["p99_ns"] for row in rows) / 1000
        updates = statistics.median(row["updates_per_second"] for row in rows)
        print(f"| {mode} | {workers} | {window} | {cpus} | {len(rows)} | {rate:.3f} | {p99:.3f} | {updates:,.0f} |")
    print("\nMedians summarize per-run measurements; tail samples are not pooled. All repetitions remain in the artifact.")


if __name__ == "__main__":
    main()
