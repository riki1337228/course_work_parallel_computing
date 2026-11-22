#!/usr/bin/env python3
"""End-to-end check of a running deployment (docker compose up).

Verifies every public contract against the real server and real Ollama:
GET /api/health, GET /api/stats, POST /api/index (validation), POST /api/search
(with and without expansion, validation errors) and WebSocket /ws/search
(three stages in order). Prints a report and exits with 1 on any failure.

    docker compose --profile tools run --rm e2e
    SERVER_URL=http://localhost:8080 python scripts/e2e_check.py
"""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request

SERVER = os.environ.get("SERVER_URL", "http://localhost:8080").rstrip("/")
WAIT_INDEX_SEC = int(os.environ.get("WAIT_INDEX_SEC", "1800"))

results: list[tuple[bool, str]] = []


def request(method: str, path: str, body=None, timeout: float = 180.0, raw: bytes | None = None):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(SERVER + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read() or b"null")
    except urllib.error.HTTPError as e:
        payload = e.read()
        try:
            return e.code, json.loads(payload)
        except json.JSONDecodeError:
            return e.code, payload.decode(errors="replace")


def check(condition: bool, name: str, detail: str = "") -> bool:
    results.append((bool(condition), name))
    print(f"[{'PASS' if condition else 'FAIL'}] {name}" + (f"  ({detail})" if detail else ""), flush=True)
    return bool(condition)


def wait_for_index() -> dict:
    deadline = time.time() + WAIT_INDEX_SEC
    while time.time() < deadline:
        try:
            status, stats = request("GET", "/api/stats", timeout=10)
            if status == 200 and stats.get("index_ready"):
                return stats
        except (urllib.error.URLError, ConnectionError, TimeoutError):
            pass
        time.sleep(5)
    raise SystemExit("index was not ready in time")


def main() -> int:
    print(f"E2E check against {SERVER}")
    stats = wait_for_index()

    status, health = request("GET", "/api/health")
    check(status == 200 and health.get("status") == "ok", "GET /api/health -> 200, status ok", json.dumps(health))
    check(isinstance(health.get("ollama"), bool), "health.ollama is boolean")
    ollama_up = bool(health.get("ollama"))

    status, stats = request("GET", "/api/stats")
    check(status == 200, "GET /api/stats -> 200", json.dumps(stats))
    check(isinstance(stats.get("documents"), int) and stats["documents"] >= 10000, "stats.documents >= 10000")
    check(isinstance(stats.get("index_build_time_ms"), (int, float)), "stats.index_build_time_ms is number")
    check(isinstance(stats.get("threads_used"), int) and stats["threads_used"] >= 1, "stats.threads_used >= 1")

    for body, name in [({"threads": 0}, "threads=0"), ({"threads": -1}, "threads=-1"), ({"threads": "4"}, "threads='4'")]:
        status, err = request("POST", "/api/index", body)
        check(status == 400 and err.get("error") == "invalid_threads", f"POST /api/index {name} -> 400")
    status, err = request("POST", "/api/index", raw=b"{oops")
    check(status == 400 and err.get("error") == "invalid_json", "POST /api/index invalid JSON -> 400")

    status, err = request("POST", "/api/search", {"query": "", "mode": "keyword"})
    check(status == 400 and err.get("error") == "invalid_query", "POST /api/search empty query -> 400")
    status, err = request("POST", "/api/search", {"query": "car", "mode": "semantic"})
    check(status == 400 and err.get("error") == "invalid_mode", "POST /api/search mode=semantic -> 400")
    status, err = request("POST", "/api/search", raw=b"not json")
    check(status == 400 and err.get("error") == "invalid_json", "POST /api/search invalid JSON -> 400")

    status, res = request("POST", "/api/search", {"query": "artificial intelligence machine learning",
                                                  "mode": "keyword", "expand": False})
    ok = status == 200 and isinstance(res.get("results"), list) and len(res["results"]) == 10
    check(ok, "POST /api/search keyword (expand=false) -> 10 results", f"time_ms={res.get('time_ms') if ok else res}")
    if ok:
        scores = [r["score"] for r in res["results"]]
        check(scores == sorted(scores, reverse=True), "results sorted by score descending")
        check(all(isinstance(r["doc_id"], str) and isinstance(r["snippet"], str) and r["snippet"]
                  for r in res["results"]), "result fields doc_id/score/snippet present")

    status, res = request("POST", "/api/search", {"query": "car", "mode": "keyword", "limit": 5})
    check(status == 200 and len(res.get("results", [])) == 5, "POST /api/search with expansion, limit=5",
          f"expansion_status={res.get('expansion_status')}, terms={res.get('expanded_terms')}, "
          f"time_ms={res.get('time_ms')}")
    if ollama_up:
        check(res.get("expansion_status") == "ok", "query expansion through Ollama returned status ok",
              f"reason={res.get('fallback_reason')}")
    else:
        check(res.get("expansion_status") == "fallback", "Ollama down -> fallback mode reported")

    try:
        import websocket  # websocket-client
    except ImportError:
        check(False, "websocket-client is installed (pip install websocket-client)")
    else:
        ws_url = SERVER.replace("http://", "ws://").replace("https://", "wss://") + "/ws/search"
        ws = websocket.create_connection(ws_url, timeout=180)
        ws.send(json.dumps({"query": "climate change glacier"}))
        frames = [json.loads(ws.recv()) for _ in range(3)]
        stages = [f.get("stage") for f in frames]
        check(stages == ["query_expansion", "keyword_search", "done"], "WebSocket stages in order", str(stages))
        check(isinstance(frames[0].get("data", {}).get("expanded_terms"), list), "WS query_expansion has terms",
              f"status={frames[0].get('data', {}).get('status')}, terms={frames[0].get('data', {}).get('expanded_terms')}")
        check(len(frames[1].get("data", {}).get("results", [])) > 0, "WS keyword_search has results")
        ws.send("garbage")
        error = json.loads(ws.recv())
        check(error.get("stage") == "error" and error["data"]["error"] == "invalid_json", "WS invalid JSON -> error")
        ws.close()

    status, page = request("GET", "/api/unknown")
    check(status == 404, "unknown route -> 404")

    failed = [name for ok, name in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
