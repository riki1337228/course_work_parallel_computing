#!/usr/bin/env python3
"""Builds benchmark charts from the CSV files produced by the benchmark scripts.

Only measured data is plotted; a chart whose CSV is missing is skipped.

    index_build_time.png   Index Build Time vs Number of Threads (mean +- std, warm cache; cold cache if measured)
    index_speedup.png      Speedup vs Number of Threads (with ideal linear speedup)
    throughput.png         Throughput vs Number of Concurrent Clients
    latency.png            Average / p95 latency vs Number of Concurrent Clients
    throughput_llm.png     Throughput with LLM query expansion (if measured)
"""

from __future__ import annotations

import csv
import os
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

OUTPUT_DIR = Path(os.environ.get("OUTPUT_DIR", Path(__file__).resolve().parent.parent / "docs" / "benchmarks"))

# Reference categorical palette (fixed order) and recessive chrome.
SERIES = ["#2a78d6", "#eb6834", "#1baf7a"]
INK, INK_2, GRID, SURFACE = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"


def read(name: str) -> list[dict]:
    path = OUTPUT_DIR / name
    if not path.is_file():
        return []
    with open(path, newline="") as f:
        return [{k: float(v) for k, v in row.items()} for row in csv.DictReader(f)]


def style(ax, title: str, xlabel: str, ylabel: str) -> None:
    ax.set_title(title, color=INK, fontsize=13, loc="left", pad=12)
    ax.set_xlabel(xlabel, color=INK_2)
    ax.set_ylabel(ylabel, color=INK_2)
    ax.set_facecolor(SURFACE)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=INK_2)


def save(fig, name: str) -> None:
    fig.patch.set_facecolor("white")
    fig.tight_layout()
    fig.savefig(OUTPUT_DIR / name, dpi=150)
    plt.close(fig)
    print(f"[plot] {OUTPUT_DIR / name}")


def grouped(rows: list[dict]) -> dict[int, list[float]]:
    groups: dict[int, list[float]] = defaultdict(list)
    for r in rows:
        groups[int(r["threads"])].append(r["time_ms"] / 1000.0)
    return dict(sorted(groups.items()))


def plot_index() -> None:
    warm = grouped(read("index_build_runs.csv"))
    if not warm:
        return
    cold = grouped(read("index_build_cold.csv"))
    threads = list(warm)
    positions = list(range(len(threads)))

    fig, ax = plt.subplots(figsize=(8, 4.6))
    series = [("Warm page cache", warm, SERIES[0], -16)]
    if cold:
        series.append(("Cold page cache (read from disk)", cold, SERIES[1], 10))
    for label, data, color, dy in series:
        xs = [positions[threads.index(t)] for t in data]
        means = [statistics.mean(v) for v in data.values()]
        stds = [statistics.stdev(v) if len(v) > 1 else 0.0 for v in data.values()]
        ax.errorbar(xs, means, yerr=stds, color=color, linewidth=2, marker="o", markersize=8,
                    markeredgecolor="white", markeredgewidth=2, capsize=4, label=label)
        ax.annotate(f"{means[0]:.1f} s", (xs[0], means[0]), textcoords="offset points", xytext=(10, 4), color=INK_2)
        ax.annotate(f"{means[-1]:.1f} s", (xs[-1], means[-1]), textcoords="offset points", xytext=(-10, dy),
                    color=INK_2, ha="right")
    ax.set_xticks(positions, [str(t) for t in threads])
    ax.set_ylim(bottom=0)
    style(ax, "Index Build Time vs Number of Threads", "Number of threads", "Build time, s (mean ± std)")
    ax.legend(frameon=False, labelcolor=INK_2)
    save(fig, "index_build_time.png")

    fig, ax = plt.subplots(figsize=(8, 4.6))
    base = statistics.mean(warm[threads[0]])
    speedup = [base / statistics.mean(v) for v in warm.values()]
    ax.plot(positions, [t / threads[0] for t in threads], color=INK_2, linestyle="--", linewidth=1.5,
            label="Ideal (linear) speedup")
    ax.plot(positions, speedup, color=SERIES[0], linewidth=2, marker="o", markersize=8,
            markeredgecolor="white", markeredgewidth=2, label="Measured speedup (warm cache)")
    for x, s in zip(positions, speedup):
        ax.annotate(f"{s:.2f}×", (x, s), textcoords="offset points", xytext=(0, 10), ha="center", color=INK_2)
    ax.set_xticks(positions, [str(t) for t in threads])
    ax.set_ylim(bottom=0)
    style(ax, "Index Build Speedup vs Number of Threads", "Number of threads", "Speedup T(1) / T(n)")
    ax.legend(frameon=False, labelcolor=INK_2)
    save(fig, "index_speedup.png")


def plot_throughput() -> None:
    rows = read("search_throughput_detail.csv") or read("search_throughput.csv")
    if not rows:
        return
    clients = [int(r["clients"]) for r in rows]
    positions = list(range(len(clients)))

    fig, ax = plt.subplots(figsize=(8, 4.6))
    rps = [r["throughput_rps"] for r in rows]
    ax.bar(positions, rps, width=0.55, color=SERIES[0])
    for x, v in zip(positions, rps):
        ax.annotate(f"{v:.0f}", (x, v), textcoords="offset points", xytext=(0, 4), ha="center", color=INK_2)
    ax.set_xticks(positions, [str(c) for c in clients])
    style(ax, "Throughput vs Number of Concurrent Clients", "Concurrent clients",
          "Throughput, requests/s (keyword search)")
    save(fig, "throughput.png")

    if "p95_ms" in rows[0]:
        fig, ax = plt.subplots(figsize=(8, 4.6))
        ax.plot(positions, [r["avg_latency_ms"] for r in rows], color=SERIES[0], linewidth=2, marker="o",
                markersize=8, markeredgecolor="white", markeredgewidth=2, label="Average latency")
        ax.plot(positions, [r["p95_ms"] for r in rows], color=SERIES[1], linewidth=2, marker="o", markersize=8,
                markeredgecolor="white", markeredgewidth=2, label="95th percentile")
        ax.set_xticks(positions, [str(c) for c in clients])
        ax.set_ylim(bottom=0)
        style(ax, "Search Latency vs Number of Concurrent Clients", "Concurrent clients", "Latency, ms")
        ax.legend(frameon=False, labelcolor=INK_2)
        save(fig, "latency.png")


def plot_llm() -> None:
    rows = read("search_throughput_llm.csv")
    if not rows:
        return
    clients = [int(r["clients"]) for r in rows]
    positions = list(range(len(clients)))
    fig, ax = plt.subplots(figsize=(8, 4.6))
    rps = [r["throughput_rps"] for r in rows]
    ax.bar(positions, rps, width=0.55, color=SERIES[1])
    for x, v, r in zip(positions, rps, rows):
        ax.annotate(f"{v:.2f} rps\n{r['avg_latency_ms'] / 1000:.1f} s avg", (x, v), textcoords="offset points",
                    xytext=(0, 4), ha="center", color=INK_2, fontsize=9)
    ax.set_xticks(positions, [str(c) for c in clients])
    ax.set_ylim(top=max(rps) * 1.3)
    style(ax, "Throughput with LLM Query Expansion (Ollama, CPU)", "Concurrent clients", "Throughput, requests/s")
    save(fig, "throughput_llm.png")


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    plot_index()
    plot_throughput()
    plot_llm()


if __name__ == "__main__":
    main()
