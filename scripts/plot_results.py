#!/usr/bin/env python3
"""Turns a bench/results/<label>/ directory into the README's charts.

Two charts, because they are the two that answer a reader's actual questions:

* **Latency CDF on a log-x axis.** A linear-x CDF wastes 95% of its width on
  the part nobody cares about. Log-x shows p50 and p99.99 in the same picture,
  which is the only way to see the shape of a tail.
* **Percentile bars per configuration.** For the before/after comparisons: the
  same six percentiles side by side, so a claim of "2x faster" can be checked
  at the tail as well as the median.

Requires matplotlib; everything else is stdlib.
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

PERCENTILE_COLUMNS = [
    ("p50_ns", "p50"),
    ("p90_ns", "p90"),
    ("p99_ns", "p99"),
    ("p999_ns", "p99.9"),
    ("p9999_ns", "p99.99"),
    ("max_ns", "max"),
]


def load_rows(results_csv: Path) -> list[dict[str, str]]:
    with results_csv.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def plot_cdf(out_dir: Path, plt) -> Path | None:
    cdfs = sorted(out_dir.glob("cdf_*.csv"))
    if not cdfs:
        return None
    fig, ax = plt.subplots(figsize=(9, 5.5))
    for path in cdfs:
        # cdf_<label>_<run>.csv -- one line per label, first run only, so the
        # chart stays readable.
        label = path.stem[len("cdf_") :].rsplit("_", 1)[0]
        if not path.stem.endswith("_1"):
            continue
        with path.open(newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
        pct = [float(r["percentile"]) for r in rows]
        ns = [int(r["latency_ns"]) for r in rows]
        # x = latency, y = percentile, log-x: the standard latency CDF.
        ax.plot(ns, pct, marker="o", markersize=3, linewidth=1.4, label=label)
    ax.set_xscale("log")
    ax.set_xlabel("latency (ns, log scale)")
    ax.set_ylabel("percentile")
    ax.set_ylim(0, 100.5)
    ax.grid(True, which="both", alpha=0.25)
    ax.legend(fontsize=8)
    ax.set_title("End-to-end command latency, open loop")
    fig.tight_layout()
    path = out_dir / "latency_cdf.png"
    fig.savefig(path, dpi=150)
    return path


def plot_percentile_bars(out_dir: Path, rows: list[dict[str, str]], plt) -> Path | None:
    if not rows:
        return None
    # One bar group per (book, pattern, loop); if a configuration was repeated,
    # take the median run, because reporting the best run is cheating.
    groups: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        key = f"{row['book']}/{row['pattern']}/{row['loop']}"
        groups.setdefault(key, []).append(row)

    labels = list(groups)
    fig, ax = plt.subplots(figsize=(10, 5.5))
    width = 0.8 / len(PERCENTILE_COLUMNS)
    for i, (column, name) in enumerate(PERCENTILE_COLUMNS):
        values = []
        for key in labels:
            runs = sorted(int(r[column]) for r in groups[key])
            values.append(runs[len(runs) // 2])
        xs = [x + i * width for x in range(len(labels))]
        ax.bar(xs, values, width=width, label=name)
    ax.set_yscale("log")
    ax.set_ylabel("latency (ns, log scale)")
    ax.set_xticks([x + 0.4 for x in range(len(labels))])
    ax.set_xticklabels(labels, rotation=20, ha="right", fontsize=8)
    ax.grid(True, axis="y", which="both", alpha=0.25)
    ax.legend(fontsize=8, ncol=len(PERCENTILE_COLUMNS))
    ax.set_title("Latency percentiles by configuration (median of runs)")
    fig.tight_layout()
    path = out_dir / "latency_percentiles.png"
    fig.savefig(path, dpi=150)
    return path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results_dir", help="a bench/results/<label> directory")
    args = parser.parse_args(argv)

    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print(
            "matplotlib is required: pip install matplotlib",
            file=sys.stderr,
        )
        return 2

    out_dir = Path(args.results_dir)
    if not out_dir.is_dir():
        print(f"no such directory: {out_dir}", file=sys.stderr)
        return 2

    results_csv = out_dir / "results.csv"
    rows = load_rows(results_csv) if results_csv.exists() else []

    made = [
        p
        for p in (plot_cdf(out_dir, plt), plot_percentile_bars(out_dir, rows, plt))
        if p
    ]
    if not made:
        print(f"nothing to plot in {out_dir}", file=sys.stderr)
        return 1
    for path in made:
        print(path)

    # A reminder rather than a nicety: a chart without its machine report is
    # a picture of a number, not a result.
    if not (out_dir / "machine.txt").exists():
        print(
            "WARNING: machine.txt is missing from this directory. "
            "Do not publish these charts without it.",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
