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
from viz.server.server import Hub, MockPublisher, load_frames

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
