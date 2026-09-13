"""The WebSocket wire protocol, in one place.

Every frame carries a `v` field and a `type` discriminant. The schema is
versioned because a schema that drifts silently produces blank panels rather
than errors, and a blank panel is indistinguishable from a quiet market.

The frame builders here are the *only* place frames are constructed, so the
TypeScript types in `viz/ui/src/protocol.ts` have exactly one thing to track.
`viz/server/tests/test_protocol.py` round-trips every frame type.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

SCHEMA_VERSION = 1

# Frame types.
BOOK = "book"
TRADES = "trades"
STATS = "stats"
TRANSPORT = "transport"
HELLO = "hello"
ERROR = "error"


def hello(
    symbol: str,
    dataset: str,
    build_sha: str,
    tap_on: bool,
    publish_hz: int,
) -> dict[str, Any]:
    """Sent once, on connect. Everything the header bar needs.

    Provenance in the first frame is deliberate: a screenshot of the dashboard
    should say which build and which dataset produced it.
    """
    return {
        "v": SCHEMA_VERSION,
        "type": HELLO,
        "symbol": symbol,
        "dataset": dataset,
        "build_sha": build_sha,
        "tap": "ON" if tap_on else "OFF",
        "publish_hz": publish_hz,
    }


def book(
    t_ns: int,
    symbol: str,
    seq: int,
    bids: list[tuple[int, int, int]],
    asks: list[tuple[int, int, int]],
    last_trade: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """A depth snapshot. Levels are `[price_ticks, qty, orders]`.

    Prices are integer ticks, never dollars: the UI formats them, and sending
    a float across the wire would reintroduce exactly the representation
    problem the whole engine avoids.
    """
    return {
        "v": SCHEMA_VERSION,
        "type": BOOK,
        "t_ns": t_ns,
        "symbol": symbol,
        "seq": seq,
        "bids": [list(level) for level in bids],
        "asks": [list(level) for level in asks],
        "last_trade": last_trade,
    }


def trades(items: list[dict[str, Any]]) -> dict[str, Any]:
    """Trades since the previous book frame, batched."""
    return {"v": SCHEMA_VERSION, "type": TRADES, "items": items}


def trade_item(
    t_ns: int, price: int, qty: int, aggressor: str, resting_id: int
) -> dict[str, Any]:
    return {
        "t_ns": t_ns,
        "price": price,
        "qty": qty,
        "aggressor": aggressor,
        "resting_id": resting_id,
    }


def stats(
    ops_per_sec: int,
    p50_ns: int,
    p99_ns: int,
    p999_ns: int,
    max_ns: int,
    orders_resting: int,
    ring_drops: int,
    schedule_misses: int,
    reader_lag: int = 0,
    reader_skipped: int = 0,
    book_resyncs: int = 0,
) -> dict[str, Any]:
    """Engine statistics, once per second.

    The loss counters are in here, and the UI shows them when non-zero,
    because a silently-lossy dashboard that looks fine is worse than no
    dashboard at all. **They are three different things and the protocol
    keeps them apart**, because merging them makes the dashboard accuse the
    engine of losing data when it was the viewer that fell behind:

    * `ring_drops` -- the engine's tap ring was full and the **engine threw
      events away**. Real data loss, at the source.
    * `reader_skipped` -- this server could not keep up and deliberately
      jumped its cursor forward to stay current. The book on screen is the
      live one; the events in between were never applied to it. The engine
      was unaffected.
    * `schedule_misses` -- the benchmark harness could not keep to its own
      arrival schedule, so its latency numbers describe the harness.

    `book_resyncs` is the consequence of the first two: a gap of either kind
    means the depth view was rebuilt from scratch, because a delta book that
    has missed a cancellation carries the phantom for ever. A rising count
    means the depth on screen is younger than the session.
    """
    return {
        "v": SCHEMA_VERSION,
        "type": STATS,
        "ops_per_sec": ops_per_sec,
        "p50_ns": p50_ns,
        "p99_ns": p99_ns,
        "p999_ns": p999_ns,
        "max_ns": max_ns,
        "orders_resting": orders_resting,
        "ring_drops": ring_drops,
        "schedule_misses": schedule_misses,
        "reader_lag": reader_lag,
        "reader_skipped": reader_skipped,
        "book_resyncs": book_resyncs,
    }


def transport(
    state: str, speed: int, event_index: int, total_events: int
) -> dict[str, Any]:
    """Echo of the current transport state."""
    return {
        "v": SCHEMA_VERSION,
        "type": TRANSPORT,
        "state": state,
        "speed": speed,
        "event_index": event_index,
        "total_events": total_events,
    }


def error(message: str) -> dict[str, Any]:
    return {"v": SCHEMA_VERSION, "type": ERROR, "message": message}


# --- transport commands, client to server -----------------------------------

VALID_TRANSPORT_COMMANDS = {"play", "pause", "step", "seek", "speed", "reset"}


@dataclass
class TransportState:
    """Replay transport.

    Transport commands go to the engine process over a separate control
    channel -- **never** by reaching into the engine's memory. The tap segment
    is one-directional by construction: the server only ever writes the
    consumer tail, and nothing else in it is writable by this process in a way
    the engine reads.
    """

    state: str = "playing"
    speed: int = 1
    event_index: int = 0
    total_events: int = 0
    pending_steps: int = 0
    seek_target: int | None = None
    log: list[dict[str, Any]] = field(default_factory=list)

    def apply(self, command: dict[str, Any]) -> str | None:
        """Applies a client command. Returns an error string, or None."""
        action = command.get("action")
        if action not in VALID_TRANSPORT_COMMANDS:
            return f"unknown transport action {action!r}"
        if action == "play":
            self.state = "playing"
        elif action == "pause":
            self.state = "paused"
        elif action == "step":
            # Step one event. Build this button first: it saves hours of
            # debugging, because "what exactly did that one message do to the
            # book" is the question you ask over and over.
            self.state = "paused"
            self.pending_steps += int(command.get("count", 1))
        elif action == "speed":
            speed = int(command.get("speed", 1))
            if speed < 1:
                return "speed must be at least 1"
            self.speed = speed
        elif action == "seek":
            target = int(command.get("event_index", 0))
            if target < 0:
                return "seek target must not be negative"
            self.seek_target = target
        elif action == "reset":
            self.state = "playing"
            self.speed = 1
            self.event_index = 0
            self.seek_target = 0
            self.pending_steps = 0
        self.log.append(dict(command))
        return None

    def frame(self) -> dict[str, Any]:
        return transport(self.state, self.speed, self.event_index,
                         self.total_events)
