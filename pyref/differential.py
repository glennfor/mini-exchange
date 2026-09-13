"""Differential testing: generate flow, run both engines, shrink any failure.

The generator, the comparison and the shrinker live together because they are
one tool: a failure is only useful if it arrives as a four-line reproducer
rather than a sixty-thousand-event diff.

Usage::

    python -m pyref.differential --seeds 1000 --length 1000 \\
        --engine build/release/tools/engine_replay
"""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from .command_io import CANCEL, NEW, REPLACE, Command, write_commands
from .reference_engine import Config, ReferenceEngine

# --- flow generation -------------------------------------------------------


def generate(seed: int, length: int) -> list[Command]:
    """Seeded random flow.

    Weighted towards adds and cancels because real equity flow is; a mix with
    50% aggressive orders would hammer the matching loop and barely touch the
    book's bookkeeping, which is the opposite of where bugs live. Prices are
    drawn from a narrow band so that crossing, partial fills and deep queues
    all happen often rather than by accident.

    The generator tracks each live order's last known price and quantity so it
    can emit *same-price size-downs* deliberately. Left to uniform randomness
    those are vanishingly rare -- a replace would have to land on exactly the
    original price -- and the one replace rule that keeps time priority would
    go untested. A mutation test that broke that rule went unnoticed until
    this was added, which is precisely the argument for mutation testing.
    """
    rng = random.Random(seed)
    commands: list[Command] = []
    live: list[int] = []
    known: dict[int, tuple[int, int]] = {}  # id -> (price, qty)
    next_id = 1

    for step in range(length):
        ts = step + 1
        roll = rng.randrange(100)
        if roll < 45 or not live:
            tif = rng.choices(
                ["Day", "IOC", "FOK", "PostOnly"], weights=[70, 12, 9, 9]
            )[0]
            order_type = "Limit"
            if tif in ("IOC", "FOK") and rng.randrange(8) == 0:
                order_type = "Market"
            cmd = Command(
                verb=NEW,
                ts=ts,
                id=next_id,
                side=rng.choice(["B", "S"]),
                price=1002000 + rng.randrange(41),
                qty=1 + rng.randrange(500),
                type=order_type,
                tif=tif,
                participant=rng.randrange(4),
            )
            next_id += 1
            live.append(cmd.id)
            known[cmd.id] = (cmd.price, cmd.qty)
        elif roll < 80:
            victim = rng.randrange(len(live))
            cmd = Command(verb=CANCEL, ts=ts, id=live[victim])
            live[victim] = live[-1]
            live.pop()
        else:
            victim = rng.randrange(len(live))
            order_id = live[victim]
            price, qty = known.get(order_id, (1002020, 100))
            shape = rng.randrange(100)
            if shape < 35 and qty > 1:
                # Same price, strictly smaller: the one change that keeps
                # time priority.
                new_price, new_qty = price, 1 + rng.randrange(qty - 1)
            elif shape < 55:
                # Same price, larger: loses priority without moving level.
                new_price, new_qty = price, qty + 1 + rng.randrange(200)
            else:
                new_price = 1002000 + rng.randrange(41)
                new_qty = 1 + rng.randrange(500)
            cmd = Command(
                verb=REPLACE, ts=ts, id=order_id, price=new_price, qty=new_qty
            )
            known[order_id] = (new_price, new_qty)
        commands.append(cmd)
    return commands


# --- running both sides ----------------------------------------------------


def reference_lines(commands: list[Command], stp: bool = False) -> list[str]:
    engine = ReferenceEngine(Config(self_trade_prevention=stp))
    engine.apply_all(commands)
    return engine.event_lines()


@dataclass
class EngineRunner:
    """Runs the compiled C++ engine as a black box.

    Nothing is imported from the C++ build and nothing is linked against it:
    the only contact between the two implementations is a text file in and a
    text file out. That is the independence requirement of spec 15.1, made
    structural rather than promised.
    """

    binary: str
    book: str = "naive"
    max_orders: int = 1 << 18

    def lines(self, commands: list[Command], stp: bool = False) -> list[str]:
        with tempfile.NamedTemporaryFile(
            "w", suffix=".cmds", delete=False, encoding="utf-8"
        ) as handle:
            handle.write(write_commands(commands))
            path = handle.name
        try:
            argv = [
                self.binary,
                "--commands",
                path,
                "--book",
                self.book,
                "--max-orders",
                str(self.max_orders),
            ]
            if stp:
                argv.append("--stp")
            result = subprocess.run(
                argv, capture_output=True, text=True, check=False
            )
            if result.returncode != 0:
                raise RuntimeError(
                    f"{self.binary} exited {result.returncode}: {result.stderr}"
                )
            return result.stdout.splitlines()
        finally:
            Path(path).unlink(missing_ok=True)


def first_difference(expected: list[str], actual: list[str]) -> int | None:
    """Index of the first differing event line, or None if identical."""
    for i in range(max(len(expected), len(actual))):
        want = expected[i] if i < len(expected) else "<end of stream>"
        got = actual[i] if i < len(actual) else "<end of stream>"
        if want != got:
            return i
    return None


def disagrees(commands: list[Command], runner: EngineRunner, stp: bool) -> bool:
    return (
        first_difference(
            reference_lines(commands, stp), runner.lines(commands, stp)
        )
        is not None
    )


# --- shrinking -------------------------------------------------------------


def shrink(
    commands: list[Command], runner: EngineRunner, stp: bool = False
) -> list[Command]:
    """Minimise a failing command sequence to a 1-minimal reproducer.

    Three stages, cheapest first:

    1. **Binary search on the prefix.** The shortest failing prefix, in
       log(n) runs rather than n. On a 1000-command failure this alone
       usually removes most of the sequence.
    2. **Chunk removal with halving chunk sizes** (delta debugging). Removing
       a quarter of the remaining commands at a time converges far faster
       than removing one at a time, and costs the same per run.
    3. **Single-command removal, repeated to a fixpoint.** One pass is not
       enough: dropping a command can make an earlier one droppable too, so
       this repeats until a whole pass changes nothing. Without the fixpoint
       the result is "smaller", which is not the same as minimal and leaves a
       reader wondering which of the sixteen lines mattered.

    The result is 1-minimal: removing any single remaining command makes the
    failure disappear. That property is asserted by the mutation tests.
    """
    # Stage 1: shortest failing prefix.
    low, high = 1, len(commands)
    while low < high:
        mid = (low + high) // 2
        if disagrees(commands[:mid], runner, stp):
            high = mid
        else:
            low = mid + 1
    current = commands[:low]

    # Stage 2: chunk removal, largest chunks first.
    chunk = len(current) // 2
    while chunk >= 1:
        i = len(current) - chunk
        while i >= 0:
            candidate = current[:i] + current[i + chunk :]
            if candidate and disagrees(candidate, runner, stp):
                current = candidate
            i -= chunk
        chunk //= 2

    # Stage 3: single commands, to a fixpoint.
    changed = True
    while changed:
        changed = False
        i = len(current) - 1
        while i >= 0:
            candidate = current[:i] + current[i + 1 :]
            if candidate and disagrees(candidate, runner, stp):
                current = candidate
                changed = True
            i -= 1
    return current


# --- reporting -------------------------------------------------------------


def report(
    seed: int,
    commands: list[Command],
    runner: EngineRunner,
    stp: bool,
    out_dir: Path,
) -> str:
    minimal = shrink(commands, runner, stp)
    expected = reference_lines(minimal, stp)
    actual = runner.lines(minimal, stp)
    index = first_difference(expected, actual)

    out_dir.mkdir(parents=True, exist_ok=True)
    repro = out_dir / f"repro_{seed}.cmds"
    repro.write_text(write_commands(minimal), encoding="utf-8")

    lines = [
        "DIFFERENTIAL FAILURE",
        f"  seed:            {seed}",
        f"  original length: {len(commands)} commands",
        f"  minimal length:  {len(minimal)} commands",
        f"  stp:             {'on' if stp else 'off'}",
        f"  first diff at:   event index {index}",
        "",
        "  minimal command sequence:",
    ]
    lines += [f"    {c.to_line()}" for c in minimal]
    lines += ["", "  expected (pyref):"]
    lines += [f"    {line}" for line in expected[: (index or 0) + 3]]
    lines += ["", "  actual (C++ engine):"]
    lines += [f"    {line}" for line in actual[: (index or 0) + 3]]
    lines += [
        "",
        f"  reproducer written to: {repro}",
        f"  rerun: {runner.binary} --commands {repro}"
        + (" --stp" if stp else ""),
        f"         python -m pyref.replay --commands {repro}"
        + (" --stp" if stp else ""),
    ]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="pyref.differential")
    parser.add_argument("--engine", required=True, help="path to engine_replay")
    parser.add_argument(
        "--book", default="naive", choices=["naive", "fast"],
        help="which C++ book to compare against",
    )
    parser.add_argument("--seeds", type=int, default=100)
    parser.add_argument("--first-seed", type=int, default=1)
    parser.add_argument("--length", type=int, default=1000)
    parser.add_argument("--stp", action="store_true")
    parser.add_argument("--out-dir", default="/tmp/miniex-repro")
    parser.add_argument(
        "--stop-after", type=int, default=1, help="stop after N failures"
    )
    args = parser.parse_args(argv)

    runner = EngineRunner(args.engine, book=args.book)
    failures = 0
    for i in range(args.seeds):
        seed = args.first_seed + i
        commands = generate(seed, args.length)
        expected = reference_lines(commands, args.stp)
        actual = runner.lines(commands, args.stp)
        if first_difference(expected, actual) is None:
            continue
        failures += 1
        print(
            report(seed, commands, runner, args.stp, Path(args.out_dir)),
            file=sys.stderr,
        )
        if failures >= args.stop_after:
            return 1

    print(
        f"# {args.seeds} sequences x {args.length} commands: "
        f"reference and C++ engine ({args.book} book) agree",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
