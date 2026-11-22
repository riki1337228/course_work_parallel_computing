#!/usr/bin/env python3
"""Experiment 2: search throughput vs number of concurrent clients.

Every client is a separate OS process (no GIL contention on the client side) with
its own persistent HTTP/1.1 connection. All clients start together (barrier) and
each sends REQUESTS_PER_CLIENT POST /api/search requests sequentially, cycling
through a fixed list of queries. Measured on the client:
    total_time_ms  wall time from start barrier to the last response
    throughput_rps successful requests / total time
    avg_latency_ms mean request latency
    errors         non-200 responses or connection errors

Part A (main): keyword search with expansion disabled -> measures the C++ engine.
Part B (optional, BENCH_LLM=1): expansion enabled -> shows the Ollama bottleneck.

Outputs (OUTPUT_DIR):
    search_throughput.csv          clients,requests,total_time_ms,throughput_rps,avg_latency_ms,errors
    search_throughput_detail.csv   + p50/p95/max latency
    search_throughput_llm.csv      same columns for Part B
    search_meta.json
"""

from __future__ import annotations

import csv
import http.client
import json
import multiprocessing as mp
import os
import statistics
import sys
import time
from urllib.parse import urlparse

from bench_common import OUTPUT_DIR, SERVER, environment_info, save_json, wait_until_ready

CLIENTS = [int(c) for c in os.environ.get("BENCH_CLIENTS", "1,5,10,20,50").split(",")]
REQUESTS_PER_CLIENT = int(os.environ.get("BENCH_REQUESTS_PER_CLIENT", "200"))
LIMIT = int(os.environ.get("BENCH_LIMIT", "10"))
RUN_LLM = os.environ.get("BENCH_LLM", "1") == "1"
LLM_CLIENTS = [int(c) for c in os.environ.get("BENCH_LLM_CLIENTS", "1,2,5,10").split(",")]
LLM_REQUESTS_PER_CLIENT = int(os.environ.get("BENCH_LLM_REQUESTS_PER_CLIENT", "3"))

QUERIES = [
    "artificial intelligence", "machine learning", "neural network training", "car engine", "electric vehicle",
    "vaccine clinical trial", "heart disease", "stock market inflation", "interest rate bank", "black hole galaxy",
    "telescope exoplanet", "pasta sauce recipe", "chocolate dessert", "football championship", "olympics medal",
    "climate change", "glacier sea level", "guitar concert", "symphony orchestra", "medieval empire",
    "industrial revolution", "thread mutex deadlock", "compiler source code", "parallel computing",
    "protein genome", "dna mutation", "court verdict", "copyright patent", "hotel flight", "museum landmark",
    "skyscraper concrete", "bridge architect", "wheat harvest irrigation", "dairy cattle", "solar panel",
    "wind turbine power grid", "university student exam", "online course", "anxiety stress memory",
    "coral reef ocean", "whale deep sea", "quantum", "car", "galaxy", "recipe",
]


def client_worker(index: int, requests: int, expand: bool, barrier, queue) -> None:
    url = urlparse(SERVER)
    conn = http.client.HTTPConnection(url.hostname, url.port or 80, timeout=600)
    latencies, errors = [], 0
    barrier.wait()
    for i in range(requests):
        query = QUERIES[(index * 7 + i) % len(QUERIES)]
        body = json.dumps({"query": query, "mode": "keyword", "limit": LIMIT, "expand": expand})
        start = time.perf_counter()
        try:
            conn.request("POST", "/api/search", body, {"Content-Type": "application/json"})
            response = conn.getresponse()
            payload = response.read()
            ok = response.status == 200 and b'"results"' in payload
        except (OSError, http.client.HTTPException):
            ok = False
            conn.close()
            conn = http.client.HTTPConnection(url.hostname, url.port or 80, timeout=600)
        elapsed = (time.perf_counter() - start) * 1000.0
        if ok:
            latencies.append(elapsed)
        else:
            errors += 1
    end = time.perf_counter()
    conn.close()
    queue.put((latencies, errors, end))


def run_level(clients: int, requests_per_client: int, expand: bool) -> dict:
    ctx = mp.get_context("spawn" if sys.platform == "win32" else "fork")
    barrier = ctx.Barrier(clients + 1)
    queue = ctx.Queue()
    procs = [ctx.Process(target=client_worker, args=(i, requests_per_client, expand, barrier, queue))
             for i in range(clients)]
    for p in procs:
        p.start()
    barrier.wait()
    start = time.perf_counter()
    outcomes = [queue.get() for _ in procs]
    for p in procs:
        p.join()
    finish = max(o[2] for o in outcomes)
    total_ms = (finish - start) * 1000.0
    latencies = [lat for o in outcomes for lat in o[0]]
    errors = sum(o[1] for o in outcomes)
    requests = clients * requests_per_client
    ordered = sorted(latencies) or [0.0]
    return {
        "clients": clients,
        "requests": requests,
        "total_time_ms": total_ms,
        "throughput_rps": len(latencies) / (total_ms / 1000.0) if total_ms > 0 else 0.0,
        "avg_latency_ms": statistics.mean(latencies) if latencies else 0.0,
        "errors": errors,
        "p50_ms": ordered[len(ordered) // 2],
        "p95_ms": ordered[min(len(ordered) - 1, int(len(ordered) * 0.95))],
        "max_ms": ordered[-1],
    }


def write(name: str, rows: list[dict], detail: bool) -> None:
    fields = ["clients", "requests", "total_time_ms", "throughput_rps", "avg_latency_ms", "errors"]
    if detail:
        fields += ["p50_ms", "p95_ms", "max_ms"]
    with open(OUTPUT_DIR / name, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(fields)
        for r in rows:
            writer.writerow([r[k] if isinstance(r[k], int) else f"{r[k]:.2f}" for k in fields])


def main() -> int:
    stats = wait_until_ready()
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    print(f"[search-bench] {stats['documents']} documents; clients={CLIENTS}, "
          f"{REQUESTS_PER_CLIENT} requests/client, expand=false", flush=True)
    run_level(4, 10, False)  # warm-up (page cache for snippets, connections)

    rows = []
    for clients in CLIENTS:
        r = run_level(clients, REQUESTS_PER_CLIENT, False)
        rows.append(r)
        print(f"[search-bench] clients={clients:3d}: {r['throughput_rps']:8.1f} rps, avg {r['avg_latency_ms']:7.2f} ms,"
              f" p95 {r['p95_ms']:7.2f} ms, errors {r['errors']}", flush=True)
    write("search_throughput.csv", rows, detail=False)
    write("search_throughput_detail.csv", rows, detail=True)

    llm_rows = []
    if RUN_LLM:
        status_ok = True
        try:
            from bench_common import http_json
            status_ok = http_json("GET", "/api/health", timeout=10)[1].get("ollama", False)
        except Exception:  # noqa: BLE001
            status_ok = False
        if not status_ok:
            print("[search-bench] Ollama unavailable - LLM part skipped", flush=True)
        else:
            print(f"[search-bench] LLM part: clients={LLM_CLIENTS}, {LLM_REQUESTS_PER_CLIENT} requests/client, "
                  "expand=true", flush=True)
            for clients in LLM_CLIENTS:
                r = run_level(clients, LLM_REQUESTS_PER_CLIENT, True)
                llm_rows.append(r)
                print(f"[search-bench] LLM clients={clients:3d}: {r['throughput_rps']:6.2f} rps, "
                      f"avg {r['avg_latency_ms']:8.1f} ms, errors {r['errors']}", flush=True)
            write("search_throughput_llm.csv", llm_rows, detail=True)

    save_json("search_meta.json", {**environment_info(), "clients": CLIENTS,
                                   "requests_per_client": REQUESTS_PER_CLIENT, "limit": LIMIT,
                                   "queries": len(QUERIES), "documents": stats["documents"],
                                   "llm_clients": LLM_CLIENTS if llm_rows else [],
                                   "llm_requests_per_client": LLM_REQUESTS_PER_CLIENT if llm_rows else 0})
    return 0


if __name__ == "__main__":
    sys.exit(main())
