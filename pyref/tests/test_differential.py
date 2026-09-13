"""The differential suite: agreement, and proof that disagreement is caught.

A differential test that has never failed proves nothing. The mutation tests
below inject known bugs -- the classic ones -- and assert that the comparison
notices *and* that the shrinker reduces the failure to something a person can
read.
"""

from __future__ import annotations

import pytest

from pyref import differential
from pyref.command_io import Command, write_commands
from pyref.differential import (
    EngineRunner,
    first_difference,
    generate,
    reference_lines,
    shrink,
)
from pyref.reference_engine import Config, ReferenceEngine

# --- agreement with the compiled engine -------------------------------------


@pytest.mark.parametrize("seed", [1, 2, 3, 17, 4711])
def test_agrees_with_cpp_engine(engine_binary, engine_book, seed):
    commands = generate(seed, 1000)
    runner = EngineRunner(engine_binary, book=engine_book)
    index = first_difference(reference_lines(commands), runner.lines(commands))
    assert index is None, f"first divergence at event index {index}"


@pytest.mark.parametrize("seed", [5, 6, 7])
def test_agrees_with_cpp_engine_with_stp(engine_binary, engine_book, seed):
    commands = generate(seed, 1000)
    runner = EngineRunner(engine_binary, book=engine_book)
    index = first_difference(
        reference_lines(commands, stp=True), runner.lines(commands, stp=True)
    )
    assert index is None, f"first divergence at event index {index}"


def test_generated_flow_actually_exercises_the_engine():
    """A generator that produces no trades would make every test above vacuous."""
    engine = ReferenceEngine(Config())
    engine.apply_all(generate(1, 2000))
    assert engine.stats.trades > 50
    assert engine.stats.rejects > 0
    kinds = {e.type for e in engine.events}
    assert {"ACK", "TRADE", "FILL", "CANCELLED", "REPLACED", "REJECT"} <= kinds


# --- mutation testing -------------------------------------------------------


class MutantRunner:
    """Stands in for the C++ engine, running a deliberately broken reference.

    Duck-typed against ``EngineRunner``: the shrinker only needs ``.lines()``.
    """

    def __init__(self, engine_factory):
        self._factory = engine_factory

    def lines(self, commands, stp=False):
        engine = self._factory(stp)
        engine.apply_all(list(commands))
        return engine.event_lines()

    binary = "<mutant>"


def _trade_at_aggressor_price_engine(stp):
    """The classic bug: print the trade at the incoming order's price."""

    class Mutant(ReferenceEngine):
        def _match(self, order_id, side, limit, qty, participant):
            # Reimplementing the loop would risk diverging for the wrong
            # reason, so instead patch the price on the way out: run the real
            # loop, then rewrite the trade prices it emitted.
            before = len(self.events)
            remaining = super()._match(order_id, side, limit, qty, participant)
            for event in self.events[before:]:
                if event.type in ("TRADE", "FILL") and limit not in (
                    2**63 - 1,
                    -(2**63) + 1,
                ):
                    event.price = limit
            return remaining

    return Mutant(Config(self_trade_prevention=stp))


def _size_down_loses_priority_engine(stp):
    """A replace bug: treat a size-down as a full requeue."""

    class Mutant(ReferenceEngine):
        def on_replace(self, cmd):
            order = self.book.find(cmd.id)
            if order is not None and cmd.price == order.price and cmd.qty <= order.qty_open:
                # Force the losing-priority path.
                cmd = Command(
                    verb=cmd.verb,
                    ts=cmd.ts,
                    id=cmd.id,
                    price=cmd.price,
                    qty=cmd.qty + 0,
                )
                side = order.side
                participant = order.participant
                self.book.cancel(cmd.id)
                from pyref.command_io import Event

                self._emit(
                    Event(
                        seq=self._next_seq(),
                        type="REPLACED",
                        id=cmd.id,
                        side=side,
                        price=cmd.price,
                        qty=cmd.qty,
                        qty_remaining=cmd.qty,
                    )
                )
                self._accept(
                    cmd.id,
                    side,
                    cmd.price,
                    cmd.qty,
                    "Limit",
                    "Day",
                    participant,
                    cmd.ts,
                )
                return
            super().on_replace(cmd)

    return Mutant(Config(self_trade_prevention=stp))


def _lifo_within_a_level_engine(stp):
    """A book bug: serve the newest order at a level first."""

    class MutantBook(ReferenceEngine):
        def _match(self, order_id, side, limit, qty, participant):
            from pyref.reference_book import opposite

            other = opposite(side)
            levels = self.book.bids if other == "B" else self.book.asks
            for queue in levels.values():
                queue.reverse()
            try:
                return super()._match(order_id, side, limit, qty, participant)
            finally:
                for queue in levels.values():
                    queue.reverse()

    return MutantBook(Config(self_trade_prevention=stp))


@pytest.mark.parametrize(
    ("name", "factory"),
    [
        ("trade at aggressor price", _trade_at_aggressor_price_engine),
        ("size-down loses priority", _size_down_loses_priority_engine),
        ("LIFO within a level", _lifo_within_a_level_engine),
    ],
)
def test_injected_bugs_are_caught_and_shrunk(name, factory):
    runner = MutantRunner(factory)

    found = None
    for seed in range(1, 30):
        commands = generate(seed, 500)
        if first_difference(reference_lines(commands), runner.lines(commands)) is not None:
            found = (seed, commands)
            break
    assert found is not None, f"the comparison failed to notice '{name}'"

    seed, commands = found
    minimal = shrink(commands, runner)

    # Still a real failure ...
    assert (
        first_difference(reference_lines(minimal), runner.lines(minimal)) is not None
    )
    # ... and small enough to read.
    assert len(minimal) <= 12, (
        f"'{name}' shrank to {len(minimal)} commands, which is too many to "
        f"read:\n{write_commands(minimal)}"
    )
    # Removing any single command must make the failure go away, or the
    # shrinker stopped early.
    for i in range(len(minimal)):
        candidate = minimal[:i] + minimal[i + 1 :]
        if candidate:
            assert not differential.disagrees(candidate, runner, False), (
                f"command {i} was not needed; the shrinker left it in"
            )


def test_shrinker_reports_a_usable_reproducer(tmp_path):
    runner = MutantRunner(_trade_at_aggressor_price_engine)
    seed = next(
        s
        for s in range(1, 30)
        if first_difference(
            reference_lines(generate(s, 500)), runner.lines(generate(s, 500))
        )
        is not None
    )
    text = differential.report(seed, generate(seed, 500), runner, False, tmp_path)
    assert "DIFFERENTIAL FAILURE" in text
    assert "minimal command sequence" in text
    assert "expected (pyref)" in text
    assert (tmp_path / f"repro_{seed}.cmds").exists()
