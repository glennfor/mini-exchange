"""CLI: run a command file through the reference engine.

Deliberately the same shape as the C++ ``engine_replay`` so the two can be
diffed with no adapter:

    python -m pyref.replay --commands cmds.txt > py.events
    ./build/release/tools/engine_replay --commands cmds.txt 2>/dev/null > cpp.events
    diff py.events cpp.events
"""

from __future__ import annotations

import argparse
import sys

from .command_io import parse_commands
from .reference_engine import Config, ReferenceEngine


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="pyref.replay",
        description="run commands through the reference matching engine",
    )
    parser.add_argument("--commands", required=True, help="command file to replay")
    parser.add_argument("--symbol", type=int, default=0)
    parser.add_argument(
        "--stp", action="store_true", help="enable self-trade prevention"
    )
    parser.add_argument(
        "--quiet", action="store_true", help="summary only, no event stream"
    )
    args = parser.parse_args(argv)

    with open(args.commands, encoding="utf-8") as handle:
        commands = parse_commands(handle.read())

    engine = ReferenceEngine(
        Config(symbol=args.symbol, self_trade_prevention=args.stp)
    )
    engine.apply_all(commands)

    if not args.quiet:
        out = sys.stdout
        for line in engine.event_lines():
            out.write(line + "\n")

    stats = engine.stats
    print(
        f"# commands={stats.commands} events={len(engine.events)} "
        f"trades={stats.trades} shares={stats.shares_traded} "
        f"rejects={stats.rejects} resting={engine.book.order_count()}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
