"""Save repeatable benchmark sweeps and the environment needed to interpret them."""
import argparse
import datetime
import json
import os
import pathlib
import platform
import subprocess


def run(command, root, required=True):
    result = subprocess.run(command, cwd=root, capture_output=True, text=True)
    if required and result.returncode:
        raise RuntimeError(f"Command failed: {command}\n{result.stderr}")
    return result.stdout.strip() if result.returncode == 0 else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--binary", default="build/forwarding_bench")
    parser.add_argument("--workers", default="1,2,4,8")
    parser.add_argument("--repetitions", default=3, type=int)
    parser.add_argument("--packets", default=1000000, type=int)
    parser.add_argument("--routes", default=1000000, type=int)
    parser.add_argument("--updates", default=100000, type=int)
    parser.add_argument("--batch", default=512, type=int)
    parser.add_argument("--window", default=2049, type=int)
    args = parser.parse_args()
    workers = [int(value) for value in args.workers.split(",")]
    if not workers or min(workers) < 1 or max(workers) > 64 or args.repetitions < 1:
        parser.error("Workers must be in 1..64 and repetitions must be positive")
    root = pathlib.Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("Output directory must be empty to preserve previous runs")
    dirty = run(["git", "status", "--porcelain"], root)
    system = platform.uname()
    metadata = {
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "revision": run(["git", "rev-parse", "HEAD"], root),
        "working_tree_changes": dirty,
        "system": system.system, "release": system.release, "machine": system.machine,
        "logical_cpus": os.cpu_count(),
        "allowed_cpus": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
        "cpu_model": run(["sysctl", "-n", "machdep.cpu.brand_string"], root, False)
                     if system.system == "Darwin" else platform.processor(),
        "compiler": run(["c++", "--version"], root),
        "build_cache": {},
        "note": "CPU affinity and background load were not controlled; no NIC I/O.",
    }
    cache = (root / args.binary).resolve().parent / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text().splitlines():
            if line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS_RELEASE:")):
                key, value = line.split("=", 1)
                metadata["build_cache"][key] = value
    output.mkdir(parents=True, exist_ok=True)
    (output / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    for mode in ("core", "queued"):
        for count in workers:
            for repetition in range(1, args.repetitions + 1):
                command = [args.binary, "--mode", mode, "--workers", str(count),
                           "--packets", str(args.packets), "--routes", str(args.routes),
                           "--updates", str(args.updates), "--batch", str(args.batch),
                           "--window", str(args.window)]
                result = json.loads(run(command, root))
                if result["errors"] != 0:
                    raise RuntimeError("Benchmark reported forwarding errors")
                result["command"] = command
                name = f"{mode}-{count}-workers-run-{repetition}.json"
                (output / name).write_text(json.dumps(result, indent=2) + "\n")
                print(f"Saved {name}", flush=True)


if __name__ == "__main__":
    main()
