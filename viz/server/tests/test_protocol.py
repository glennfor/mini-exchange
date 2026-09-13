"""The wire schema. Every frame type is built exactly once, here, and the
TypeScript types in the UI track this file.
"""

from __future__ import annotations

import json

from viz.server import protocol


def roundtrip(frame: dict) -> dict:
    """Every frame must survive JSON encoding, because that is what happens
    to it. A tuple that a test compares equal to a list will not."""
    return json.loads(json.dumps(frame))


def test_every_frame_carries_a_version_and_a_type():
    frames = [
        protocol.hello("AAPL", "itch:20190130", "abc1234", True, 30),
        protocol.book(1, "AAPL", 2, [(1002100, 400, 3)], [(1002400, 100, 1)]),
        protocol.trades([protocol.trade_item(1, 1002400, 100, "B", 8821)]),
        protocol.stats(6120000, 118, 342, 910, 14200, 48211, 0, 3),
        protocol.transport("playing", 10, 918273, 41882910),
        protocol.error("nope"),
    ]
    for frame in frames:
        decoded = roundtrip(frame)
        assert decoded["v"] == protocol.SCHEMA_VERSION
        assert isinstance(decoded["type"], str) and decoded["type"]


def test_book_levels_survive_json_as_lists():
    frame = roundtrip(
        protocol.book(
            34200123456789, "AAPL", 918273,
            [(1002100, 400, 3), (1002000, 1200, 7)],
            [(1002400, 100, 1)],
            {"price": 1002400, "qty": 100, "side": "B"},
        )
    )
    assert frame["bids"] == [[1002100, 400, 3], [1002000, 1200, 7]]
    assert frame["asks"] == [[1002400, 100, 1]]
    assert frame["last_trade"]["price"] == 1002400


def test_prices_are_integer_ticks_not_dollars():
    frame = protocol.book(1, "AAPL", 1, [(1002100, 400, 3)], [])
    price = frame["bids"][0][0]
    assert isinstance(price, int), (
        "sending a float price would reintroduce exactly the representation "
        "problem the engine exists to avoid; the UI formats ticks into dollars"
    )


def test_stats_carries_drops_and_misses():
    frame = roundtrip(protocol.stats(1, 2, 3, 4, 5, 6, 7, 8, 9))
    assert frame["ring_drops"] == 7
    assert frame["schedule_misses"] == 8
    assert frame["reader_lag"] == 9


# --- transport ---------------------------------------------------------------


def test_play_pause_and_speed():
    t = protocol.TransportState()
    assert t.apply({"action": "pause"}) is None
    assert t.state == "paused"
    assert t.apply({"action": "play"}) is None
    assert t.state == "playing"
    assert t.apply({"action": "speed", "speed": 100}) is None
    assert t.speed == 100


def test_step_pauses_and_queues_exactly_one_event():
    t = protocol.TransportState(state="playing")
    assert t.apply({"action": "step"}) is None
    assert t.state == "paused"
    assert t.pending_steps == 1
    t.apply({"action": "step", "count": 5})
    assert t.pending_steps == 6


def test_seek_and_reset():
    t = protocol.TransportState(state="paused", speed=10, event_index=500)
    assert t.apply({"action": "seek", "event_index": 42}) is None
    assert t.seek_target == 42
    assert t.apply({"action": "reset"}) is None
    assert (t.state, t.speed, t.event_index) == ("playing", 1, 0)


def test_bad_commands_are_rejected_with_a_message():
    t = protocol.TransportState()
    assert "unknown transport action" in (t.apply({"action": "frobnicate"}) or "")
    assert "speed must be" in (t.apply({"action": "speed", "speed": 0}) or "")
    assert "negative" in (t.apply({"action": "seek", "event_index": -1}) or "")


def test_transport_frame_reflects_state():
    t = protocol.TransportState(state="paused", speed=10, event_index=7,
                                total_events=99)
    frame = roundtrip(t.frame())
    assert frame["state"] == "paused"
    assert frame["speed"] == 10
    assert frame["event_index"] == 7
    assert frame["total_events"] == 99


def test_loss_counters_are_three_separate_fields():
    """Merging them made the dashboard blame the engine for the viewer's lag.

    `ring_drops` is the engine discarding events; `reader_skipped` is this
    server jumping forward to stay current; `schedule_misses` is the harness
    failing to keep its own schedule. Different causes, different fixes,
    different components at fault.
    """
    frame = roundtrip(
        protocol.stats(
            ops_per_sec=1_000_000,
            p50_ns=100,
            p99_ns=200,
            p999_ns=300,
            max_ns=400,
            orders_resting=10,
            ring_drops=0,
            schedule_misses=0,
            reader_lag=140_000,
            reader_skipped=7_300_000,
        )
    )
    assert frame["ring_drops"] == 0, "the engine lost nothing"
    assert frame["reader_skipped"] == 7_300_000, "the viewer skipped"
    assert frame["schedule_misses"] == 0
