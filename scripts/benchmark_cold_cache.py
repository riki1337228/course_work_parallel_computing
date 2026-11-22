#!/usr/bin/env python3
"""Supplementary experiment: index build with a COLD page cache (disk-bound).

Runs on the Docker HOST (needs the docker CLI). Before every build the Linux page
cache of the Docker VM is dropped with a privileged helper container, so all 2 GiB
of documents are read from the virtual disk. Compare with index_build.csv (warm
cache) to see how much of the build time is I/O.

    python scripts/benchmark_cold_cache.py            # SERVER_URL defaults to http://localhost:8080

Output: docs/benchmarks/index_build_cold.csv  (threads,run,time_ms,documents_indexed)
"""

from __future__ import annotations

import csv
import os
import subprocess
import sys

from bench_common import OUTPUT_DIR, http_json, wait_until_ready

THREADS = [int(t) for t in os.environ.get("BENCH_THREADS", "1,2,4,8,16").split(",")]
REPEATS = int(os.environ.get("BENCH_REPEATS", "2"))


def drop_caches() -> None:
    subprocess.run(["docker", "run", "--rm", "--privileged", "alpine:3.20", "sh", "-c",
                    "sync && echo 3 > /proc/sys/vm/drop_caches"], check=True, capture_output=True)


def main() -> int:
    wait_until_ready()
    rows = []
    for threads in THREADS:
        for run in range(1, REPEATS + 1):
            drop_caches()
            status, body = http_json("POST", "/api/index", {"threads": threads})
            if status != 200:
                raise SystemExit(f"HTTP {status}: {body}")
            rows.append({"threads": threads, "run": run, "time_ms": body["time_ms"],
                         "documents_indexed": body["documents_indexed"]})
            print(f"[cold-cache] threads={threads:2d} run={run}: {body['time_ms']:9.1f} ms", flush=True)
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    with open(OUTPUT_DIR / "index_build_cold.csv", "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["threads", "run", "time_ms", "documents_indexed"])
        writer.writeheader()
        writer.writerows(rows)
    http_json("POST", "/api/index", {"threads": 8})  # restore default state (warm)
    return 0


if __name__ == "__main__":
    sys.exit(main())
