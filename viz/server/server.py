"""The visualization server: shm tap in, WebSocket out.

Responsibilities, in order of importance:

1. **Coalesce.** A full ITCH day is tens of millions of events. Forwarding
   them per-event to a browser is not a performance problem to be optimised,
   it is a design error. The server drains the tap as fast as it can, applies
   every event to its own book, and publishes a *snapshot* at a fixed rate
   (30 Hz by default). The browser never sees an event it cannot draw.
2. **Never stall the engine.** The server only ever advances the tap's
   consumer tail. If it falls behind, the engine drops records and counts
   them; the drop count is published and the UI shows it in red.
3. **Own the transport.** Play, pause, step-one-event, speed, seek. Transport
   commands go to the engine over a separate control channel, never by
   reaching into the engine's memory.

Run it:

    python -m viz.server.server --shm /miniex_bench --port 8765
    python -m viz.server.server --mock viz/server/sample_frames.jsonl
"""

from __future__ import annotations

import argparse
import asyncio
import gzip
import json
import sys
import time
from typing import Any

from . import protocol
from .book_state import BookState
from .shm_reader import ShmTapReader, TapVersionError


def load_frames(path: str) -> list[dict[str, Any]]:
    """Reads a frame recording, gzipped or not.

    The committed sample is gzipped: 808 KB of JSON becomes 56 KB, and a
    repository that has to clone in under five minutes should not carry the
    uncompressed version of a file it can decompress in milliseconds.
    """
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as handle:
        return [json.loads(line) for line in handle if line.strip()]


class Hub:
    """Fans published frames out to every connected client."""

    def __init__(self) -> None:
        self._clients: set[Any] = set()

    def add(self, ws: Any) -> None:
        self._clients.add(ws)

    def remove(self, ws: Any) -> None:
        self._clients.discard(ws)

    @property
    def count(self) -> int:
        return len(self._clients)

    async def broadcast(self, frame: dict[str, Any]) -> None:
        """Sends one frame to every client.

        Two things this has to get right, both of which it got wrong first.

        **Snapshot the client set.** `ws.send()` suspends, and while it is
        suspended the connection handler runs -- `add()` for a new browser
        tab, `remove()` in its `finally` for a closing one. Iterating the live
        set across that await raises `RuntimeError: Set changed size during
        iteration`, which propagates out of the publisher task and kills the
        server. The symptom was the process dying the moment a second tab was
        opened.

        **Send concurrently.** Awaiting each client in turn makes the publish
        cycle cost the *sum* of every client's send, so one tab that has
        stopped reading stalls the dashboard for everyone else. Gathering
        makes it cost the slowest one instead. Frames stay ordered per client
        because the gather completes before the next broadcast starts.
        """
        clients = tuple(self._clients)
        if not clients:
            return
        payload = json.dumps(frame, separators=(",", ":"))
        results = await asyncio.gather(
            *(ws.send(payload) for ws in clients), return_exceptions=True
        )
        for ws, result in zip(clients, results, strict=True):
            # A failed send means the client is gone. Discard rather than
            # remove: it may already have been dropped by its own handler
            # while this broadcast was in flight.
            if isinstance(result, BaseException):
                self._clients.discard(ws)


class TapPublisher:
    """Drains the tap, maintains the book, publishes at a fixed rate."""

    def __init__(
        self,
        reader: ShmTapReader,
        hub: Hub,
        *,
        symbol: str = "SYNTH",
        levels: int = 20,
        publish_hz: int = 30,
        batch: int = 32768,
        max_lag: int = 200000,
    ) -> None:
        self.reader = reader
        self.hub = hub
        self.symbol = symbol
        self.levels = levels
        self.publish_hz = publish_hz
        self.batch = batch
        self.max_lag = max_lag
        self.skipped = 0
        self.book = BookState()
        self.transport = protocol.TransportState()
        self._last_stats = 0.0
        self._events_at_last_stats = 0
        self._time_at_last_stats = time.monotonic()

    async def run(self) -> None:
        interval = 1.0 / self.publish_hz
        while True:
            start = time.monotonic()
            self._drain()
            await self._publish()
            elapsed = time.monotonic() - start
            await asyncio.sleep(max(0.0, interval - elapsed))

    def _drain(self) -> None:
        """Applies as many records as are waiting, up to a bounded batch.

        Bounded so that one slow publish cycle cannot turn into an unbounded
        one: falling behind is survivable (the engine drops), spending a
        second inside one iteration is not, because the UI would freeze.
        """
        if self.transport.state == "paused" and self.transport.pending_steps <= 0:
            return
        budget = self.batch
        if self.transport.pending_steps > 0:
            budget = min(budget, self.transport.pending_steps)
        elif self.reader.lag > self.max_lag:
            # Too far behind to catch up. Skip forward rather than show a
            # lagging replay while claiming to be live; the skip is counted
            # and published.
            self.skipped += self.reader.skip_to_recent(budget)

        records = self.reader.read(budget)
        for record in records:
            self.book.apply(record)
            self.transport.event_index = record.seq
        if self.transport.pending_steps > 0:
            self.transport.pending_steps -= len(records)

    async def _publish(self) -> None:
        if self.hub.count == 0:
            # Nothing connected: keep draining (so the engine does not fill
            # its ring) but do not spend time serialising frames nobody wants.
            self.book.take_trades()
            return

        bids, asks = self.book.depth(self.levels)
        await self.hub.broadcast(
            protocol.book(
                self.book.t_ns, self.symbol, self.book.seq, bids, asks,
                self.book.last_trade,
            )
        )
        trades = self.book.take_trades()
        if trades:
            await self.hub.broadcast(protocol.trades(trades))

        now = time.monotonic()
        if now - self._last_stats >= 1.0:
            await self.hub.broadcast(self._stats_frame(now))
            await self.hub.broadcast(self.transport.frame())
            self._last_stats = now

    def _stats_frame(self, now: float) -> dict[str, Any]:
        snapshot = self.reader.read_stats()
        applied = self.book.events_applied
        window = max(1e-6, now - self._time_at_last_stats)
        observed_rate = int((applied - self._events_at_last_stats) / window)
        self._events_at_last_stats = applied
        self._time_at_last_stats = now
        return protocol.stats(
            ops_per_sec=snapshot.ops_per_sec if snapshot else observed_rate,
            p50_ns=snapshot.p50_ns if snapshot else 0,
            p99_ns=snapshot.p99_ns if snapshot else 0,
            p999_ns=snapshot.p999_ns if snapshot else 0,
            max_ns=snapshot.max_ns if snapshot else 0,
            orders_resting=self.book.orders_resting,
            ring_drops=self.reader.dropped + self.skipped,
            schedule_misses=snapshot.schedule_misses if snapshot else 0,
            reader_lag=self.reader.lag,
        )


class MockPublisher:
    """Replays a recorded JSONL of frames. No C++ build required.

    This is what unblocks UI development: the whole front end can be built and
    demonstrated on a machine with no market data, no compiler and no engine
    running.
    """

    def __init__(self, path: str, hub: Hub, *, publish_hz: int = 30,
                 loop_forever: bool = True) -> None:
        self.hub = hub
        self.publish_hz = publish_hz
        self.loop_forever = loop_forever
        self.frames = load_frames(path)
        if not self.frames:
            raise ValueError(f"{path} contains no frames")
        self.transport = protocol.TransportState(
            total_events=sum(1 for f in self.frames if f.get("type") == "book")
        )

    async def run(self) -> None:
        interval = 1.0 / self.publish_hz
        index = 0
        while True:
            if self.hub.count == 0 or self.transport.state == "paused":
                if self.transport.pending_steps > 0:
                    self.transport.pending_steps -= 1
                else:
                    await asyncio.sleep(interval)
                    continue
            frame = dict(self.frames[index])
            if frame.get("type") == "book":
                self.transport.event_index = int(frame.get("seq", 0))
            await self.hub.broadcast(frame)
            index += 1
            if index >= len(self.frames):
                if not self.loop_forever:
                    return
                index = 0
            await asyncio.sleep(interval / max(1, self.transport.speed))


async def serve(args: argparse.Namespace) -> int:
    try:
        import websockets
    except ImportError:
        print(
            "the websockets package is required: pip install websockets",
            file=sys.stderr,
        )
        return 2

    hub = Hub()
    publisher: TapPublisher | MockPublisher

    if args.mock:
        publisher = MockPublisher(args.mock, hub, publish_hz=args.hz)
        dataset = f"mock:{args.mock}"
        tap_on = False
    else:
        try:
            reader = ShmTapReader(args.shm)
        except TapVersionError as exc:
            print(f"server: {exc}", file=sys.stderr)
            return 3
        except FileNotFoundError as exc:
            print(f"server: {exc}", file=sys.stderr)
            return 3
        publisher = TapPublisher(
            reader, hub, symbol=args.symbol, levels=args.levels,
            publish_hz=args.hz, batch=args.batch, max_lag=args.max_lag,
        )
        dataset = f"shm:{args.shm}"
        tap_on = True
        print(
            f"# attached to {args.shm}: capacity {reader.capacity} records, "
            f"producer {'live' if reader.producer_live else 'NOT running'}",
            file=sys.stderr,
        )

    async def handler(ws: Any) -> None:
        hub.add(ws)
        await ws.send(
            json.dumps(
                protocol.hello(args.symbol, dataset, args.build_sha, tap_on,
                               args.hz)
            )
        )
        await ws.send(json.dumps(publisher.transport.frame()))
        try:
            async for message in ws:
                try:
                    command = json.loads(message)
                except json.JSONDecodeError:
                    await ws.send(json.dumps(protocol.error("bad JSON")))
                    continue
                err = publisher.transport.apply(command)
                if err:
                    await ws.send(json.dumps(protocol.error(err)))
                await hub.broadcast(publisher.transport.frame())
        except Exception:  # a dropped client is not an error
            pass
        finally:
            hub.remove(ws)

    print(f"# listening on ws://{args.host}:{args.port}", file=sys.stderr)
    async with websockets.serve(handler, args.host, args.port):
        await publisher.run()
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="viz.server.server")
    parser.add_argument("--shm", default="/miniex_bench",
                        help="tap segment name")
    parser.add_argument("--mock", default="",
                        help="replay this JSONL instead of attaching to a tap")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--hz", type=int, default=30,
                        help="publish rate; the whole point of the server")
    parser.add_argument("--levels", type=int, default=20)
    parser.add_argument("--symbol", default="SYNTH")
    parser.add_argument("--batch", type=int, default=32768,
                        help="records drained per publish cycle")
    parser.add_argument("--max-lag", type=int, default=200000,
                        help="skip forward past this much backlog rather than "
                             "showing a lagging replay")
    parser.add_argument("--build-sha", default="unknown")
    args = parser.parse_args(argv)

    try:
        return asyncio.run(serve(args))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
