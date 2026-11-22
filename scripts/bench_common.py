"""Shared helpers for benchmark scripts (standard library only)."""

from __future__ import annotations

import json
import os
import platform
import time
import urllib.error
import urllib.request
from pathlib import Path

SERVER = os.environ.get("SERVER_URL", "http://localhost:8080").rstrip("/")
OUTPUT_DIR = Path(os.environ.get("OUTPUT_DIR", Path(__file__).resolve().parent.parent / "docs" / "benchmarks"))


def http_json(method: str, path: str, body=None, timeout: float = 3600.0):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(SERVER + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def wait_until_ready(timeout_sec: int = 3600) -> dict:
    """Waits until the index is built and no build is running; returns /api/stats."""
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        try:
            status, stats = http_json("GET", "/api/stats", timeout=10)
            if status == 200 and stats.get("index_ready") and not stats.get("indexing_in_progress"):
                return stats
        except (urllib.error.URLError, ConnectionError, TimeoutError):
            pass
        time.sleep(3)
    raise SystemExit(f"server {SERVER} is not ready")


def environment_info() -> dict:
    return {
        "server": SERVER,
        "client_platform": platform.platform(),
        "client_cpu_count": os.cpu_count(),
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }


def save_json(name: str, payload: dict) -> Path:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    path = OUTPUT_DIR / name
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return path
