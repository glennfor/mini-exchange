"""The C++ engine writes, Python reads. No mocks anywhere in this file.

This is the test that would catch a real layout or semantics mismatch between
the two processes -- the kind that `test_layout.py` catches statically, and
this one catches dynamically, on data an actual matching engine produced.
"""

from __future__ import annotations

import contextlib
import os
import subprocess
from pathlib import Path

import pytest

from viz.server.book_state import BookState
from viz.server.shm_reader import (
    EVENT_ACK,
    EVENT_TRADE,
    ShmTapReader,
)

_PRODUCERS = [
    "build/dev/tools/tap_producer",
    "build/release/tools/tap_producer",
]


def _find_producer() -> str | None:
    override = os.environ.get("MINIEX_TAP_PRODUCER")
    if override:
        return override if Path(override).exists() else None
    root = Path(__file__).resolve().parents[3]
    for candidate in _PRODUCERS:
        path = root / candidate
        if path.exists():
            return str(path)
    return None


@pytest.fixture
def tap_segment(request):
    """Runs the producer to completion, then hands the reader the segment."""
    producer = _find_producer()
    if producer is None:
        pytest.skip(
            "tap_producer not built; run "
            "`cmake --preset dev && cmake --build build/dev`"
        )
    name = f"/miniex_pytest_{os.getpid()}_{abs(hash(request.node.name)) % 10000}"
    result = subprocess.run(
        [
            producer, "--shm", name, "--capacity", "1048576",
            "--commands", "40000", "--warmup", "5000", "--rate", "0",
            "--seed", "11",
        ],
        capture_output=True, text=True, check=False,
    )
    assert result.returncode == 0, result.stderr
    yield name
    # Best effort: the reader's close() unmaps, and the name goes with the
    # process that made it on a normal exit.
    with contextlib.suppress(OSError):
        os.unlink(f"/dev/shm{name}")


def test_reader_attaches_and_sees_what_the_engine_wrote(tap_segment):
    with ShmTapReader(tap_segment) as reader:
        assert reader.capacity == 1048576
        assert reader.produced > 0

        records = reader.read(1048576)
        assert len(records) > 1000

        # Sequence numbers are contiguous from zero: the engine assigns them
        # and the ring preserves order.
        for i, record in enumerate(records):
            assert record.seq == i, f"gap at index {i}"


def test_the_event_stream_has_the_shape_the_engine_promises(tap_segment):
    with ShmTapReader(tap_segment) as reader:
        records = reader.read(1048576)

    kinds = {r.type_name for r in records}
    assert "ACK" in kinds
    assert "TRADE" in kinds
    assert "FILL" in kinds

    # Every trade names a resting order and prints at a real price.
    trades = [r for r in records if r.type == EVENT_TRADE]
    assert trades
    for trade in trades[:200]:
        assert trade.counter_id != 0, "a trade must name the resting order"
        assert trade.qty > 0
        assert trade.price > 0
        assert trade.side_char in ("B", "S")


def test_prices_and_quantities_survive_the_crossing_intact(tap_segment):
    """A byte-order or field-offset mistake would show up here as absurd
    values rather than as an exception."""
    with ShmTapReader(tap_segment) as reader:
        records = reader.read(1048576)

    acks = [r for r in records if r.type == EVENT_ACK and not (r.flags & 1)]
    assert acks
    for ack in acks[:500]:
        # The synthetic flow is centred on 1002000 ticks with a bounded drift.
        assert 900_000 < ack.price < 1_100_000, (
            f"price {ack.price} is nowhere near the generator's range; "
            "suspect a byte-order or offset mismatch"
        )
        assert 0 < ack.qty <= 1000
        assert ack.ts > 0


def test_the_server_book_reconstructs_a_sane_market(tap_segment):
    with ShmTapReader(tap_segment) as reader:
        records = reader.read(1048576)

    book = BookState()
    for record in records:
        book.apply(record)

    bids, asks = book.depth(10)
    assert bids, "the book should not be empty after 40000 commands"
    assert asks

    # Descending bids, ascending asks, and no negative quantities anywhere.
    assert [level[0] for level in bids] == sorted(
        (level[0] for level in bids), reverse=True
    )
    assert [level[0] for level in asks] == sorted(level[0] for level in asks)
    for level in bids + asks:
        assert level[1] > 0
        assert level[2] > 0

    assert book.orders_resting > 0
    assert book.events_applied == len(records)


def test_draining_advances_the_tail_so_a_restart_sees_nothing_twice(tap_segment):
    with ShmTapReader(tap_segment) as reader:
        first = reader.read(1000)
        assert len(first) == 1000
        second = reader.read(1000)
        assert len(second) == 1000
        assert second[0].seq == first[-1].seq + 1
        assert reader.tail == 2000


def test_a_full_ring_reports_drops_rather_than_losing_them_silently():
    """The engine drops when the ring fills, and says how many.

    A dashboard that quietly shows a partial market is worse than one that
    says it is missing data, which is why the drop count is part of the
    protocol and shown in red.
    """
    producer = _find_producer()
    if producer is None:
        pytest.skip("tap_producer not built")
    name = f"/miniex_pytest_drops_{os.getpid()}"
    result = subprocess.run(
        [producer, "--shm", name, "--capacity", "1024", "--commands", "50000",
         "--warmup", "2000", "--rate", "0"],
        capture_output=True, text=True, check=False,
    )
    assert result.returncode == 0, result.stderr
    try:
        with ShmTapReader(name) as reader:
            assert reader.dropped > 0
            assert reader.produced == 1024, (
                "with nothing draining, exactly one ring's worth is retained"
            )
    finally:
        with contextlib.suppress(OSError):
            os.unlink(f"/dev/shm{name}")


def test_attaching_live_passes_over_the_backlog_without_calling_it_loss(
    tap_segment,
):
    """A dashboard joining a running engine starts at 'now'.

    The ring holds history the dashboard was not present for. Replaying it
    shows a stale market and then races to catch up -- and because the
    catch-up exceeds the lag ceiling, it also gets counted as a skip, so the
    panel showed a permanent "the viewer is behind" warning for something
    that happened once, at startup, by design.
    """
    with ShmTapReader(tap_segment) as reader:
        waiting = reader.head - reader.tail
        assert waiting > 1000, "the fixture should leave a real backlog"

        skipped = reader.seek_to_live()
        assert skipped == waiting
        assert reader.lag == 0
        assert reader.read(100) == [], "nothing left until the producer writes"

        # And it is not folded into the drop counter, which is the engine's.
        assert reader.dropped == 0
