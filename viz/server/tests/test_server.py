"""The server end to end: a real WebSocket client against a real server.

Covers the two things the server exists for -- coalescing and transport --
and the one thing it must never do, which is stall the engine.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import time
from pathlib import Path

import pytest

from viz.server import protocol
from viz.server.server import Hub, MockPublisher, TapPublisher, load_frames
from viz.server.shm_reader import (
    EVENT_ACK,
    EVENT_CANCELLED,
    EVENT_FILL,
    TapRecord,
)

websockets = pytest.importorskip("websockets")

SAMPLE = (
    Path(__file__).resolve().parents[1] / "sample_frames.jsonl.gz"
)


def test_the_committed_sample_recording_is_readable_and_well_formed():
    assert SAMPLE.exists(), (
        "the gzipped sample recording is committed so the UI can be developed "
        "and demonstrated with no C++ build present"
    )
    frames = load_frames(str(SAMPLE))
    assert len(frames) > 100

    kinds = {f["type"] for f in frames}
    assert {"hello", "book", "trades", "stats", "transport"} <= kinds
    for frame in frames:
        assert frame["v"] == protocol.SCHEMA_VERSION

    books = [f for f in frames if f["type"] == "book"]
    assert len(books) > 100
    for book in books[:50]:
        bids = [level[0] for level in book["bids"]]
        asks = [level[0] for level in book["asks"]]
        assert bids == sorted(bids, reverse=True)
        assert asks == sorted(asks)
        if bids and asks:
            assert bids[0] < asks[0], "the sample book must not be crossed"


def test_stats_frames_have_a_plausible_distribution_shape():
    # A UI drawn against a flat latency distribution will have a useless x
    # axis, so the fixture has to have a tail.
    frames = load_frames(str(SAMPLE))
    stats = [f for f in frames if f["type"] == "stats"]
    assert stats
    for s in stats:
        assert s["p50_ns"] < s["p99_ns"] < s["p999_ns"] < s["max_ns"]
        assert s["ops_per_sec"] > 0


class FakeSocket:
    def __init__(self) -> None:
        self.sent: list[dict] = []

    async def send(self, payload: str) -> None:
        self.sent.append(json.loads(payload))


@pytest.mark.asyncio
async def test_hub_fans_out_and_drops_dead_clients():
    hub = Hub()
    good = FakeSocket()

    class Dead(FakeSocket):
        async def send(self, payload: str) -> None:
            raise ConnectionError("gone")

    hub.add(good)
    hub.add(Dead())
    assert hub.count == 2

    await hub.broadcast(protocol.error("hello"))
    assert len(good.sent) == 1
    assert hub.count == 1, "a client whose send failed must be dropped"


@pytest.mark.asyncio
async def test_mock_publisher_replays_frames_to_a_client():
    hub = Hub()
    client = FakeSocket()
    hub.add(client)
    publisher = MockPublisher(str(SAMPLE), hub, publish_hz=1000,
                              loop_forever=False)

    task = asyncio.create_task(publisher.run())
    await asyncio.sleep(0.4)
    task.cancel()
    with contextlib.suppress(asyncio.CancelledError):
        await task

    assert len(client.sent) > 10
    assert {f["type"] for f in client.sent} & {"book", "trades", "stats"}


@pytest.mark.asyncio
async def test_pause_stops_the_stream_and_step_advances_exactly_one_frame():
    hub = Hub()
    client = FakeSocket()
    hub.add(client)
    publisher = MockPublisher(str(SAMPLE), hub, publish_hz=1000)
    publisher.transport.apply({"action": "pause"})

    task = asyncio.create_task(publisher.run())
    await asyncio.sleep(0.15)
    assert client.sent == [], "a paused transport must publish nothing"

    publisher.transport.apply({"action": "step"})
    await asyncio.sleep(0.15)
    task.cancel()
    with contextlib.suppress(asyncio.CancelledError):
        await task

    assert len(client.sent) == 1, (
        "step must advance exactly one frame -- this is the button that saves "
        "hours of debugging, and it is worthless if it advances two"
    )


@pytest.mark.asyncio
async def test_a_client_connecting_mid_broadcast_does_not_kill_the_server():
    """The crash this reproduces killed the whole server.

    `await ws.send(...)` suspends, and while it is suspended the connection
    handler runs -- `hub.add()` for a new browser tab, or `hub.remove()` in
    its `finally` for a closing one. Iterating the live set across that await
    raises `RuntimeError: Set changed size during iteration`, which propagates
    out of the publisher task and takes the process with it.

    Symptom in the field: the server died the moment a second tab was opened.
    """
    hub = Hub()

    class ConnectsAnother(FakeSocket):
        async def send(self, payload: str) -> None:
            # Exactly what the real handler does while a broadcast is
            # suspended on this very send.
            hub.add(FakeSocket())
            await super().send(payload)

    hub.add(ConnectsAnother())
    await hub.broadcast(protocol.error("tick"))
    assert hub.count == 2


@pytest.mark.asyncio
async def test_a_client_disconnecting_mid_broadcast_does_not_kill_the_server():
    hub = Hub()
    victim = FakeSocket()

    class DisconnectsAnother(FakeSocket):
        async def send(self, payload: str) -> None:
            hub.remove(victim)
            await super().send(payload)

    hub.add(DisconnectsAnother())
    hub.add(victim)
    await hub.broadcast(protocol.error("tick"))
    assert hub.count == 1


@pytest.mark.asyncio
async def test_one_slow_client_does_not_delay_the_others():
    """A dashboard tab that reads slowly must not hold up every other tab.

    Sends go out concurrently, so the publish cycle costs the slowest client
    once rather than the sum of all of them.
    """
    hub = Hub()
    fast = FakeSocket()

    class Slow(FakeSocket):
        async def send(self, payload: str) -> None:
            await asyncio.sleep(0.05)
            await super().send(payload)

    hub.add(Slow())
    hub.add(fast)

    start = asyncio.get_running_loop().time()
    await hub.broadcast(protocol.error("tick"))
    elapsed = asyncio.get_running_loop().time() - start

    assert len(fast.sent) == 1
    assert elapsed < 0.09, (
        "two clients, one of them 50 ms slow: concurrent sends cost ~50 ms, "
        f"serial sends cost ~100 ms; measured {elapsed*1000:.0f} ms"
    )


@pytest.mark.asyncio
async def test_a_paused_transport_stops_the_chart_rather_than_repeating_itself():
    """Pausing must stop the heatmap, not feed it copies of one column.

    The publisher used to broadcast a book frame every cycle regardless. The
    heatmap treats each book frame as one time column, so a paused transport
    drew a wall of identical columns scrolling right -- straight horizontal
    lines, looking like a market frozen at a price rather than a stopped
    replay.
    """
    hub = Hub()
    client = FakeSocket()
    hub.add(client)
    publisher = MockPublisher(str(SAMPLE), hub, publish_hz=1000)

    task = asyncio.create_task(publisher.run())
    await asyncio.sleep(0.15)
    publisher.transport.apply({"action": "pause"})
    books_at_pause = sum(1 for f in client.sent if f["type"] == "book")
    assert books_at_pause > 0, "it should have been publishing before the pause"

    # Sampled twice, and the assertion is on the second interval. The
    # publisher reads the transport once per cycle, so a pause landing
    # mid-cycle lets exactly one already-started frame out; asserting on the
    # first interval made this test fail about one run in five for a frame
    # that is not the bug. What must hold is that the count stops *growing*.
    await asyncio.sleep(0.1)
    books_settled = sum(1 for f in client.sent if f["type"] == "book")
    await asyncio.sleep(0.2)
    books_after = sum(1 for f in client.sent if f["type"] == "book")
    task.cancel()
    with contextlib.suppress(asyncio.CancelledError):
        await task

    assert books_settled - books_at_pause <= 1, (
        f"{books_settled - books_at_pause} book frames arrived after the "
        "pause; at most the one cycle already in flight may escape"
    )
    assert books_after == books_settled, (
        f"{books_after - books_settled} book frames arrived while paused; a "
        "paused transport must publish no new time columns at all"
    )


def test_a_publisher_exposes_its_current_book_for_a_client_joining_paused():
    """Otherwise a client that connects while paused sees an empty screen
    forever, because the only frames that carry the book are the ones the
    pause is suppressing."""
    hub = Hub()
    publisher = MockPublisher(str(SAMPLE), hub, publish_hz=1000)
    assert publisher.book_frame() is None, "nothing published yet"

    # After a frame has gone out, the publisher can hand it to a latecomer.
    publisher._last_book = {"v": 1, "type": "book", "seq": 7}
    assert publisher.book_frame()["seq"] == 7


class FakeReader:
    """A tap that can be told to lose events, so the gap path is testable."""

    def __init__(self, cost_per_record: float = 0.0) -> None:
        self.queue: list[TapRecord] = []
        self.dropped = 0
        self.lag = 0
        self.cost_per_record = cost_per_record

    def read(self, budget: int) -> list[TapRecord]:
        out, self.queue = self.queue[:budget], self.queue[budget:]
        if self.cost_per_record and out:
            deadline = time.monotonic() + self.cost_per_record * len(out)
            while time.monotonic() < deadline:
                pass
        return out

    def skip_to_recent(self, keep: int) -> int:
        return 0

    def read_stats(self):
        return None


def _record(seq, etype, order_id, price, qty, side):
    return TapRecord(
        seq=seq, ts=seq * 1000, price=price, qty=qty, qty_remaining=0,
        id=order_id, counter_id=0, level_index=0, type=etype, side=side,
        reason=0, flags=0,
    )


def test_a_drop_in_the_tap_makes_the_server_rebuild_its_book():
    """The bug this closes was visible on screen for minutes at a time.

    The dashboard showed bids at 100.2195 against asks at 100.1840 -- crossed
    by 355 ticks, and stuck. A price-time-priority engine cannot hold a
    crossed book, so what was on screen was this server's reconstruction of
    one, with orders in it whose cancellations had been dropped by a full tap
    ring. The reconstruction had no way back: a phantom level never leaves.

    It now throws the book away whenever the stream has a hole in it.
    """
    reader = FakeReader()
    publisher = TapPublisher(reader, Hub())

    reader.queue = [_record(1, EVENT_ACK, 1, 1002195, 500, 0)]
    publisher._drain()
    assert publisher.book.depth(5)[0] == [[1002195, 500, 1]]

    # The cancellation that would have taken that bid away is dropped, and an
    # ask arrives far below it.
    reader.dropped += 12
    reader.queue = [_record(3, EVENT_ACK, 2, 1001840, 500, 1)]
    publisher._drain()

    assert publisher.book.resyncs == 1
    assert not publisher.book.crossed, (
        "a crossed book is proof the reconstruction lost events; it must not "
        "be published as though it were the engine's book"
    )
    assert publisher.book.depth(5) == ([], [[1001840, 500, 1]])


def test_an_uninterrupted_stream_is_never_rebuilt():
    # The counter has to mean something, so it must not tick on a clean run.
    reader = FakeReader()
    publisher = TapPublisher(reader, Hub())
    reader.queue = [
        _record(1, EVENT_ACK, 1, 1002000, 500, 0),
        _record(2, EVENT_ACK, 2, 1002100, 500, 1),
        _record(3, EVENT_CANCELLED, 1, 1002000, 500, 0),
    ]
    for _ in range(3):
        publisher._drain()
    assert publisher.book.resyncs == 0
    assert publisher.book.depth(5) == ([], [[1002100, 500, 1]])


def test_a_reader_that_starts_behind_does_not_count_as_a_gap():
    # Attaching to an engine that has been running does not mean *this*
    # server lost anything: the book is empty, and there is nothing to throw
    # away. Counting it would report a rebuild for every session.
    reader = FakeReader()
    reader.dropped = 48_976
    publisher = TapPublisher(reader, Hub())
    reader.queue = [_record(1, EVENT_ACK, 1, 1002000, 500, 0)]
    publisher._drain()
    assert publisher.book.resyncs == 0


def test_draining_stops_at_the_deadline_so_publishing_keeps_its_cadence():
    """The failure: five heatmap columns in thirty seconds.

    A tap record costs about 2.5 us to unpack and apply in Python, so a full
    32768-record batch costs 80 ms against a 33 ms publish interval. Once the
    reader was behind enough to fill a batch every cycle it stayed there --
    every cycle overran, the publish rate collapsed to a few hertz, and the
    heatmap (one column per published frame) went nearly blank while the
    engine ran at full speed and every component worked as designed.
    """
    reader = FakeReader(cost_per_record=20e-6)
    publisher = TapPublisher(reader, Hub())
    reader.queue = [
        _record(i, EVENT_ACK, i, 1002000 + (i % 7), 100, i % 2)
        for i in range(20_000)
    ]

    start = time.monotonic()
    publisher._drain(deadline=start + 0.02)
    elapsed = time.monotonic() - start

    assert elapsed < 0.05, (
        f"drained for {elapsed*1000:.0f} ms against a 20 ms deadline; the "
        "publish loop cannot keep 30 Hz if one drain eats several intervals"
    )
    assert publisher.book.events_applied > 0, "it must still make progress"
    assert reader.queue, "and it must have stopped early, not finished"


def test_a_drain_with_no_deadline_still_respects_the_record_budget():
    reader = FakeReader()
    publisher = TapPublisher(reader, Hub(), batch=100)
    reader.queue = [
        _record(i, EVENT_ACK, i, 1002000, 100, 0) for i in range(1000)
    ]
    publisher._drain()
    assert publisher.book.events_applied == 100


def test_a_half_applied_command_is_held_back_rather_than_drawn():
    """The engine Acks an order before it matches it.

    Between that Ack and the trades that follow, this server's reconstruction
    has the aggressor resting at a price that crosses the other side. It is a
    real state of the event stream and not a state the engine's book was ever
    in -- and a deadline-bounded drain stops inside that window often enough
    to matter: one published frame in twenty-five, measured against the live
    engine, showed a locked or crossed market.
    """
    hub = Hub()
    client = FakeSocket()
    hub.add(client)
    reader = FakeReader()
    publisher = TapPublisher(reader, hub)

    reader.queue = [_record(1, EVENT_ACK, 1, 1002000, 500, 1)]  # an ask
    publisher._drain()
    asyncio.run(publisher._publish())
    published = [f for f in client.sent if f["type"] == "book"]
    assert len(published) == 1

    # A bid arrives that crosses it. Its Ack lands; its trades have not been
    # drained yet.
    reader.queue = [_record(2, EVENT_ACK, 2, 1002050, 500, 0)]
    publisher._drain()
    asyncio.run(publisher._publish())
    assert publisher.book.crossed
    assert [f for f in client.sent if f["type"] == "book"] == published, (
        "a crossed book must not be published; the next cycle will finish "
        "the command"
    )

    # The rest of the command arrives and the frame goes out.
    reader.queue = [_record(3, EVENT_FILL, 2, 1002050, 500, 0)]
    publisher._drain()
    asyncio.run(publisher._publish())
    assert not publisher.book.crossed
    assert len([f for f in client.sent if f["type"] == "book"]) == 2


def test_a_crossing_that_does_not_clear_is_treated_as_lost_events():
    # Otherwise holding the frame back would freeze the display for ever on
    # the one failure it cannot fix by waiting.
    hub = Hub()
    hub.add(FakeSocket())
    reader = FakeReader()
    publisher = TapPublisher(reader, hub, crossed_limit=3)
    reader.queue = [
        _record(1, EVENT_ACK, 1, 1002000, 500, 1),
        _record(2, EVENT_ACK, 2, 1002050, 500, 0),
    ]
    publisher._drain()
    for _ in range(3):
        asyncio.run(publisher._publish())
    assert publisher.book.resyncs == 1
    assert not publisher.book.crossed


@pytest.mark.asyncio
async def test_a_client_that_has_stopped_reading_is_dropped_not_waited_for():
    """The hang this closes looked exactly like a dead server.

    Gathering the sends costs the slowest client, and "slowest" has no upper
    bound: a tab Chrome has backgrounded stops draining its socket, TCP flow
    control backs the send up, and the publish loop waits on it for ever.
    Every other tab stops, the once-a-second stats stop, and the process sits
    there at 50% CPU draining the tap and publishing to nobody.

    The test above covered a client that was *slow*. This one covers a client
    that has *stopped*, which is the case that actually happened.
    """
    hub = Hub(send_timeout=0.05)
    healthy = FakeSocket()

    class Stopped(FakeSocket):
        closed = False

        async def send(self, payload: str) -> None:
            await asyncio.Event().wait()  # never completes

        async def close(self) -> None:
            self.closed = True

    stopped = Stopped()
    hub.add(stopped)
    hub.add(healthy)

    start = asyncio.get_running_loop().time()
    await hub.broadcast(protocol.error("tick"))
    elapsed = asyncio.get_running_loop().time() - start

    assert len(healthy.sent) == 1, "the healthy client must still get its frame"
    assert elapsed < 0.5, (
        f"broadcast took {elapsed*1000:.0f} ms waiting on a client that will "
        "never read; a market feed does not wait for a subscriber"
    )
    assert hub.count == 1, "the stopped client must be dropped"
    assert stopped.closed, "and closed, not just forgotten"
    assert hub.dropped_slow == 1


@pytest.mark.asyncio
async def test_a_dropped_client_does_not_take_the_publisher_with_it():
    # The drop path raises so the caller discards the client; that exception
    # must not escape broadcast() and kill the publish task.
    hub = Hub(send_timeout=0.01)

    class Stopped(FakeSocket):
        async def send(self, payload: str) -> None:
            await asyncio.Event().wait()

        async def close(self) -> None:
            pass

    hub.add(Stopped())
    await hub.broadcast(protocol.error("tick"))
    await hub.broadcast(protocol.error("tick"))
    assert hub.count == 0


def test_a_client_connecting_mid_command_is_not_handed_a_crossed_book():
    """The one snapshot the publish loop does not produce.

    `book_frame()` goes out from the connection handler, on connect, so it
    bypassed the check in `_publish` -- and against the live engine, roughly
    one connect in twenty-five landed inside the window between an Ack and
    the trades that follow it.
    """
    reader = FakeReader()
    publisher = TapPublisher(reader, Hub())
    reader.queue = [_record(1, EVENT_ACK, 1, 1002000, 500, 1)]
    publisher._drain()
    good = publisher.book_frame()
    assert good is not None and good["asks"] == [[1002000, 500, 1]]

    reader.queue = [_record(2, EVENT_ACK, 2, 1002050, 500, 0)]
    publisher._drain()
    assert publisher.book.crossed
    assert publisher.book_frame() == good, (
        "a latecomer gets the last coherent book, not a half-applied command"
    )


def test_a_client_connecting_before_any_book_gets_nothing_rather_than_a_lie():
    publisher = TapPublisher(FakeReader(), Hub())
    publisher.book.apply(_record(1, EVENT_ACK, 1, 1002000, 500, 1))
    publisher.book.apply(_record(2, EVENT_ACK, 2, 1002050, 500, 0))
    assert publisher.book.crossed
    assert publisher.book_frame() is None
