"""Records a JSONL of wire frames, for the mock server and the UI.

Runs the engine's *synthetic* flow through the same `BookState` the live
server uses and writes the frames it would have published. The result is a
small file that lets the whole front end be built, tested and demonstrated on
a machine with no C++ build, no market data and no engine running.

    python -m viz.server.record_frames --out viz/server/sample_frames.jsonl \
        --seconds 30

It deliberately reuses `protocol.py` rather than writing frames by hand: a
recorded sample that drifts from the live schema would send UI developers
chasing bugs that do not exist.
"""

from __future__ import annotations

import argparse
import gzip
import json
import random
import sys
from typing import Any

from . import protocol


def generate(seed: int, seconds: int, hz: int, levels: int,
             symbol: str) -> list[dict[str, Any]]:
    """A plausible book and tape, generated directly at the frame level.

    Not a matching engine: this is a *fixture* for the UI, and building one
    out of the real engine would mean the UI's test data could only be
    produced by a working C++ build, which is the dependency this is here to
    remove.
    """
    rng = random.Random(seed)
    frames: list[dict[str, Any]] = []
    frames.append(
        protocol.hello(symbol, f"synthetic:seed={seed}", "sample", False, hz)
    )

    mid = 1002000
    t_ns = 34_200_000_000_000
    seq = 0
    total = seconds * hz
    # Persistent resting size per level, so the heatmap shows structure that
    # persists rather than noise. A book that is re-randomised every frame
    # looks like static and teaches a UI developer nothing.
    bid_sizes = {mid - 1 - i: rng.randrange(100, 4000) for i in range(levels)}
    ask_sizes = {mid + 1 + i: rng.randrange(100, 4000) for i in range(levels)}
    last_trade = None

    for frame_index in range(total):
        t_ns += 1_000_000_000 // hz

        # A slow random walk in the mid, with occasional jumps, so the
        # heatmap and the ladder both have something to show.
        if rng.random() < 0.04:
            step = rng.choice([-3, -2, -1, 1, 2, 3])
            mid += step
            bid_sizes = {
                mid - 1 - i: bid_sizes.get(mid - 1 - i,
                                           rng.randrange(100, 4000))
                for i in range(levels)
            }
            ask_sizes = {
                mid + 1 + i: ask_sizes.get(mid + 1 + i,
                                           rng.randrange(100, 4000))
                for i in range(levels)
            }

        # Churn a few levels.
        for sizes in (bid_sizes, ask_sizes):
            for _ in range(3):
                price = rng.choice(list(sizes))
                sizes[price] = max(0, sizes[price] +
                                   rng.randrange(-800, 900))

        trades: list[dict[str, Any]] = []
        for _ in range(rng.choices([0, 1, 2, 5], weights=[55, 30, 10, 5])[0]):
            aggressor = "B" if rng.random() < 0.5 else "S"
            price = mid + 1 if aggressor == "B" else mid - 1
            qty = rng.choice([100, 100, 200, 300, 500, 1500])
            trades.append(
                protocol.trade_item(t_ns, price, qty, aggressor,
                                    rng.randrange(1000, 99999))
            )
            last_trade = {"price": price, "qty": qty, "side": aggressor}
            sizes = ask_sizes if aggressor == "B" else bid_sizes
            if price in sizes:
                sizes[price] = max(0, sizes[price] - qty)

        seq += 1 + len(trades) * 2
        bids = [[p, q, max(1, q // 250)]
                for p, q in sorted(bid_sizes.items(), reverse=True) if q > 0]
        asks = [[p, q, max(1, q // 250)]
                for p, q in sorted(ask_sizes.items()) if q > 0]
        frames.append(
            protocol.book(t_ns, symbol, seq, bids[:levels], asks[:levels],
                          last_trade)
        )
        if trades:
            frames.append(protocol.trades(trades))

        if frame_index % hz == 0:
            # Percentiles with a plausible shape: a tight body and a tail that
            # is an order of magnitude out, because that is what a real
            # latency distribution looks like and a UI drawn against a flat
            # one will have a useless x axis.
            p50 = rng.randrange(95, 140)
            frames.append(
                protocol.stats(
                    ops_per_sec=rng.randrange(4_000_000, 7_000_000),
                    p50_ns=p50,
                    p99_ns=p50 * rng.randrange(3, 5),
                    p999_ns=p50 * rng.randrange(8, 14),
                    max_ns=p50 * rng.randrange(80, 300),
                    orders_resting=sum(1 for q in bid_sizes.values() if q > 0)
                    + sum(1 for q in ask_sizes.values() if q > 0),
                    ring_drops=0,
                    schedule_misses=rng.choice([0, 0, 0, 0, 3]),
                    reader_lag=rng.randrange(0, 4000),
                )
            )
            frames.append(protocol.transport("playing", 1, seq, total * 10))

    return frames


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="viz.server.record_frames")
    parser.add_argument("--out", required=True)
    parser.add_argument("--seed", type=int, default=20260913)
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--hz", type=int, default=30)
    parser.add_argument("--levels", type=int, default=20)
    parser.add_argument("--symbol", default="SYNTH")
    parser.add_argument("--gzip", action="store_true",
                        help="write a .gz (the committed sample is gzipped)")
    args = parser.parse_args(argv)

    frames = generate(args.seed, args.seconds, args.hz, args.levels,
                      args.symbol)
    payload = "".join(
        json.dumps(f, separators=(",", ":")) + "\n" for f in frames
    )
    if args.gzip:
        with gzip.open(args.out, "wt", encoding="utf-8") as handle:
            handle.write(payload)
    else:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(payload)
    print(f"# wrote {len(frames)} frames to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
