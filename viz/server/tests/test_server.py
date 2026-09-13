"""The server end to end: a real WebSocket client against a real server.

Covers the two things the server exists for -- coalescing and transport --
and the one thing it must never do, which is stall the engine.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
from pathlib import Path

import pytest

from viz.server import protocol
from viz.server.server import Hub, MockPublisher, TapPublisher, load_frames
from viz.server.shm_reader import EVENT_ACK, EVENT_CANCELLED, TapRecord

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
async def test_one_stalled_client_does_not_delay_the_others():
    """A dashboard tab that stops reading must not hold up every other tab.

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

    def __init__(self) -> None:
        self.queue: list[TapRecord] = []
        self.dropped = 0
        self.lag = 0

    def read(self, budget: int) -> list[TapRecord]:
        out, self.queue = self.queue[:budget], self.queue[budget:]
        return out

    def skip_to_recent(self, keep: int) -> int:
        return 0


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
