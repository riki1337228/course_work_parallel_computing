#!/usr/bin/env python3
"""Experiment 1: index build time vs number of threads.

For every thread count (default 1,2,4,8,16) the index is rebuilt through
POST /api/index several times (default 3). The server measures the build time
(listing + parallel read/tokenize/merge + parallel sort) and returns it in time_ms.
One warm-up build is executed first so that every measured run sees the same
(warm) OS page cache; cold-cache behaviour is measured separately by
scripts/benchmark_cold_cache.py.

Outputs (OUTPUT_DIR, default docs/benchmarks):
    index_build.csv          threads,time_ms,documents_indexed        (mean per thread count)
    index_build_runs.csv     threads,run,time_ms,documents_indexed    (every run)
    index_build_summary.csv  threads,mean_ms,std_ms,min_ms,max_ms,speedup,efficiency
    index_build_meta.json    parameters and environment
"""

from __future__ import annotations

import csv
import os
import statistics
import sys

from bench_common import OUTPUT_DIR, environment_info, http_json, save_json, wait_until_ready

THREADS = [int(t) for t in os.environ.get("BENCH_THREADS", "1,2,4,8,16").split(",")]
REPEATS = int(os.environ.get("BENCH_REPEATS", "3"))
RESTORE_THREADS = int(os.environ.get("BENCH_RESTORE_THREADS", "8"))


def build(threads: int) -> dict:
    status, body = http_json("POST", "/api/index", {"threads": threads})
    if status != 200:
        raise SystemExit(f"POST /api/index threads={threads} failed: HTTP {status} {body}")
    return body


def main() -> int:
    stats = wait_until_ready()
    print(f"[index-bench] dataset: {stats['documents']} documents; threads={THREADS}, repeats={REPEATS}", flush=True)

    warm = build(max(THREADS))
    print(f"[index-bench] warm-up build: {warm['time_ms']:.1f} ms", flush=True)

    runs = []
    for threads in THREADS:
        for run in range(1, REPEATS + 1):
            body = build(threads)
            runs.append({"threads": threads, "run": run, "time_ms": body["time_ms"],
                         "documents_indexed": body["documents_indexed"]})
            print(f"[index-bench] threads={threads:2d} run={run}: {body['time_ms']:9.1f} ms "
                  f"({body['documents_indexed']} docs)", flush=True)

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    with open(OUTPUT_DIR / "index_build_runs.csv", "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["threads", "run", "time_ms", "documents_indexed"])
        writer.writeheader()
        writer.writerows(runs)

    summary = []
    for threads in THREADS:
        times = [r["time_ms"] for r in runs if r["threads"] == threads]
        docs = [r["documents_indexed"] for r in runs if r["threads"] == threads][0]
        summary.append({"threads": threads, "mean": statistics.mean(times),
                        "std": statistics.stdev(times) if len(times) > 1 else 0.0,
                        "min": min(times), "max": max(times), "documents_indexed": docs})
    base = next(s["mean"] for s in summary if s["threads"] == THREADS[0])

    with open(OUTPUT_DIR / "index_build.csv", "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["threads", "time_ms", "documents_indexed"])
        for s in summary:
            writer.writerow([s["threads"], f"{s['mean']:.1f}", s["documents_indexed"]])

    with open(OUTPUT_DIR / "index_build_summary.csv", "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["threads", "mean_ms", "std_ms", "min_ms", "max_ms", "speedup", "efficiency"])
        for s in summary:
            speedup = base / s["mean"]
            writer.writerow([s["threads"], f"{s['mean']:.1f}", f"{s['std']:.1f}", f"{s['min']:.1f}",
                             f"{s['max']:.1f}", f"{speedup:.2f}", f"{speedup / s['threads'] * THREADS[0]:.2f}"])
            print(f"[index-bench] threads={s['threads']:2d}: mean {s['mean']:9.1f} ms  speedup {speedup:5.2f}",
                  flush=True)

    save_json("index_build_meta.json", {**environment_info(), "threads": THREADS, "repeats": REPEATS,
                                        "warmup_ms": warm["time_ms"], "documents": stats["documents"],
                                        "unique_terms": stats["unique_terms"],
                                        "total_postings": stats["total_postings"]})
    if RESTORE_THREADS > 0:
        build(RESTORE_THREADS)  # leave the server in its default configuration
    return 0


if __name__ == "__main__":
    sys.exit(main())
