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
import math
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


def _percentile_to_y(p: float) -> float:
    """Compresses the percentile axis towards 100.

    A linear percentile axis gives half its height to the region between p0
    and p50, where nothing interesting happens, and squeezes p99 to p100 --
    the entire point of a latency chart -- into the top 1% of the pixels.
    This maps p to log10(100/(100-p)), so each decade of "nines" gets equal
    space: p90, p99, p99.9 and p99.99 are evenly spaced.

    The divisor is 5, not 4, so that p99.99 lands at 0.8 and the maximum has
    the top of the axis to itself. With 4 they both mapped to 1.0 and their
    tick labels drew on top of each other.
    """
    if p >= 100:
        return 1.0
    if p <= 0:
        return 0.0
    return min(1.0, math.log10(100.0 / (100.0 - p)) / 5.0)


def plot_cdf(out_dir: Path, plt) -> Path | None:
    """Latency CDF: log-x, nines-compressed y, open-loop series only.

    Service-time runs (offered rate 0) are excluded rather than drawn in a
    lighter shade: they are a *different quantity* -- dequeue to handler
    return, with no queueing in them at all -- and putting them on the same
    axes as end-to-end latency invites exactly the comparison that must not be
    made. The closed-loop run is included, dashed and labelled, because the
    gap between it and its open-loop twin is the single most informative thing
    in the chart.
    """
    cdfs = sorted(out_dir.glob("cdf_*_1.csv"))
    if not cdfs:
        return None

    series = []
    for path in cdfs:
        label = path.stem[len("cdf_") :].rsplit("_", 1)[0]
        if "throughput" in label:
            continue  # service time, not latency
        with path.open(newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
        points = [
            (int(r["latency_ns"]), float(r["percentile"]))
            for r in rows
            if int(r["latency_ns"]) > 0
        ]
        if points:
            series.append((label, points))
    if not series:
        return None

    fig, ax = plt.subplots(figsize=(11, 6))
    colours = plt.get_cmap("tab20")(range(20))
    for i, (label, points) in enumerate(sorted(series)):
        closed = "closed-loop" in label
        ax.plot(
            [ns for ns, _ in points],
            [_percentile_to_y(p) for _, p in points],
            marker="o",
            markersize=2.5,
            linewidth=2.0 if closed else 1.3,
            linestyle="--" if closed else "-",
            color=colours[i % 20],
            label=label + (" (comparison only)" if closed else ""),
        )

    ax.set_xscale("log")
    ax.set_xlabel("latency (ns, log scale)")
    ax.set_ylabel("percentile")
    ticks = [0, 50, 90, 99, 99.9, 99.99, 100]
    ax.set_yticks([_percentile_to_y(p) for p in ticks])
    ax.set_yticklabels(["0", "p50", "p90", "p99", "p99.9", "p99.99", "max"])
    ax.set_ylim(0, 1.02)
    ax.grid(True, which="both", alpha=0.25)
    ax.legend(fontsize=7, loc="center left", bbox_to_anchor=(1.01, 0.5),
              frameon=False)
    ax.set_title(
        "End-to-end command latency, open loop\n"
        "development machine -- 41 ns timer, loaded desktop; read ratios, "
        "not absolutes",
        fontsize=10,
    )
    fig.tight_layout()
    path = out_dir / "latency_cdf.png"
    fig.savefig(path, dpi=150)
    return path


def plot_percentile_bars(out_dir: Path, rows: list[dict[str, str]], plt) -> Path | None:
    if not rows:
        return None
    # One bar group per (book, pattern, loop), and **service-time rows are
    # excluded**. They carry loop="open" like everything else, so grouping
    # without checking `measure` silently averaged dequeue-to-return samples
    # together with end-to-end ones -- which moved NaiveBook's p50 from 458 ns
    # to 300 ns and would have been a quietly wrong chart next to a correct
    # table.
    #
    # Where a configuration was repeated, the median run is used. Reporting
    # the best run is cheating.
    groups: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        if row.get("measure") == "service_time":
            continue
        key = f"{row['book']}/{row['pattern']}/{row['loop']}"
        groups.setdefault(key, []).append(row)
    if not groups:
        return None

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
    ax.set_title(
        "End-to-end latency percentiles by configuration (median of runs)\n"
        "development machine -- 41 ns timer, loaded desktop; read ratios, "
        "not absolutes",
        fontsize=10,
    )
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
