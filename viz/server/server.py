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
import contextlib
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

    #: How long one client may take to accept a frame before it is dropped.
    #: Several publish intervals, so an ordinary hiccup is tolerated and a
    #: stopped consumer is not.
    SEND_TIMEOUT_S = 0.5

    def __init__(self, send_timeout: float = SEND_TIMEOUT_S) -> None:
        self._clients: set[Any] = set()
        self.send_timeout = send_timeout
        self.dropped_slow = 0

    def add(self, ws: Any) -> None:
        self._clients.add(ws)

    def remove(self, ws: Any) -> None:
        self._clients.discard(ws)

    @property
    def count(self) -> int:
        return len(self._clients)

    async def _send(self, ws: Any, payload: str) -> None:
        """One send, with a deadline.

        A timeout cancels the send part way through a frame, so the client's
        stream is no longer coherent and the connection has to go. It is
        closed rather than merely forgotten, or the socket would sit in the
        event loop holding a buffer nobody will ever read.
        """
        try:
            await asyncio.wait_for(ws.send(payload), self.send_timeout)
        except asyncio.TimeoutError:
            self.dropped_slow += 1
            with contextlib.suppress(Exception):
                await asyncio.wait_for(ws.close(), 0.1)
            raise

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

        **Bound the wait.** Gathering costs the slowest client, and "slowest"
        has no upper bound: a tab Chrome has backgrounded stops draining its
        socket entirely, TCP flow control backs the send up, and the publish
        loop waits on it for ever. Every other tab, and the stats frames with
        it, stop dead -- which looked exactly like a hung server and was one
        stalled consumer. The suite had a test for this that only covered a
        client that was *slow*, not one that had *stopped*, so it passed.

        A market feed does not wait for a subscriber. A client that cannot
        take a frame within several publish intervals is dropped, the same
        way the engine's tap drops rather than stalling matching.
        """
        clients = tuple(self._clients)
        if not clients:
            return
        payload = json.dumps(frame, separators=(",", ":"))
        results = await asyncio.gather(
            *(self._send(ws, payload) for ws in clients),
            return_exceptions=True,
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
        chunk: int = 2048,
        drain_share: float = 0.6,
        crossed_limit: int = 30,
        max_trades_per_frame: int = 512,
    ) -> None:
        self.reader = reader
        self.hub = hub
        self.symbol = symbol
        self.levels = levels
        self.publish_hz = publish_hz
        self.batch = batch
        self.max_lag = max_lag
        self.chunk = chunk
        self.drain_share = drain_share
        self.crossed_limit = crossed_limit
        self.max_trades_per_frame = max_trades_per_frame
        self.skipped = 0
        self._drops_seen = reader.dropped
        self.book = BookState()
        self._last_published_seq: int | None = None
        self._crossed_cycles = 0
        self._last_frame: dict[str, Any] | None = None
        self.transport = protocol.TransportState()
        self._last_stats = 0.0
        self._events_at_last_stats = 0
        self._time_at_last_stats = time.monotonic()

    async def run(self) -> None:
        interval = 1.0 / self.publish_hz
        while True:
            start = time.monotonic()
            self._drain(deadline=start + interval * self.drain_share)
            await self._publish()
            elapsed = time.monotonic() - start
            await asyncio.sleep(max(0.0, interval - elapsed))

    def _drain(self, deadline: float | None = None) -> None:
        """Applies what is waiting, bounded by a record count *and* a clock.

        The record bound alone was not enough, and the way it failed is worth
        keeping. Unpacking and applying one tap record costs about 2.5 us in
        Python, so a full 32768-record batch costs 80 ms -- against a 33 ms
        publish interval. Once the reader was far enough behind to fill a
        batch every cycle it stayed there: every cycle overran, the publish
        rate fell to a few hertz, and the heatmap, which draws one column per
        published frame, showed five columns in thirty seconds while the
        engine ran at full speed. The dashboard looked broken while every
        component in it was working.

        The clock bound breaks that. Draining stops when the cycle's share of
        the interval is spent, so publishing keeps its cadence, and because
        the drain rate that fits in that share (~240k records/s) exceeds what
        the engine produces, the reader catches up instead of settling into
        the overrun.
        """
        if self.transport.state == "paused" and self.transport.pending_steps <= 0:
            return
        budget = self.batch
        gap = False
        if self.transport.pending_steps > 0:
            budget = min(budget, self.transport.pending_steps)
        elif self.reader.lag > self.max_lag:
            # Too far behind to catch up. Skip forward rather than show a
            # lagging replay while claiming to be live; the skip is counted
            # and published.
            jumped = self.reader.skip_to_recent(budget)
            self.skipped += jumped
            gap = gap or jumped > 0

        # The engine discards rather than blocking when its ring fills, so a
        # rising drop count is a hole in this stream. Either way the book
        # below is now missing the events that would have taken orders out of
        # it, and a delta book with holes in it is not a book -- see
        # book_state.py. Throw it away here, before the post-gap records are
        # applied on top of it.
        drops = self.reader.dropped
        if drops != self._drops_seen:
            self._drops_seen = drops
            gap = True
        if gap:
            self.book.resync()

        # Read in chunks so the deadline can be honoured part way through a
        # batch rather than only between batches.
        applied = 0
        while applied < budget:
            chunk = min(self.chunk, budget - applied)
            records = self.reader.read(chunk)
            if not records:
                break
            for record in records:
                self.book.apply(record)
            self.transport.event_index = records[-1].seq
            applied += len(records)
            if len(records) < chunk:
                break  # caught up
            if deadline is not None and time.monotonic() >= deadline:
                break
        if self.transport.pending_steps > 0:
            self.transport.pending_steps -= applied

    def book_frame(self) -> dict[str, Any] | None:
        """The current depth snapshot. Sent to a client the moment it
        connects, so a client that joins while the transport is paused sees
        the book rather than an empty screen.

        Returns the last coherent snapshot while the reconstruction is
        mid-command -- see `_publish` for why that happens. Going straight to
        `depth()` here was the one path that could still hand a client a
        crossed book, because it is the one snapshot the publish loop does not
        produce: it goes out from the connection handler, on connect.
        """
        if self.book.crossed:
            return self._last_frame
        self._last_frame = protocol.book(
            self.book.t_ns, self.symbol, self.book.seq,
            *self.book.depth(self.levels), self.book.last_trade,
        )
        return self._last_frame

    async def _publish(self) -> None:
        if self.hub.count == 0:
            # Nothing connected: keep draining (so the engine does not fill
            # its ring) but do not spend time serialising frames nobody wants.
            self.book.take_trades()
            return

        # Only publish a book frame when the book actually moved.
        #
        # Publishing an unchanged snapshot every cycle is not harmless: the
        # heatmap treats each book frame as one time column, so a paused
        # transport produced a wall of identical columns scrolling to the
        # right -- straight horizontal lines, as if the market had frozen at
        # a price rather than stopped. Pausing must stop the chart, not feed
        # it copies.
        #
        # And never publish a crossed book.
        #
        # The engine emits an order's Ack before it matches it, so between
        # that Ack and the trades that follow, this reconstruction has the
        # aggressor resting at a price that crosses the other side. It is a
        # real intermediate state of the *event stream*, but it is not a state
        # the engine's book was ever in, and a drain that stops inside that
        # window -- which a deadline-bounded drain sometimes does -- would
        # publish it. Measured at about one frame in twenty-five.
        #
        # Holding the frame back costs at most one publish interval, because
        # the next drain finishes the command. If it does not clear, the cause
        # is not a half-applied command but lost events, and the only repair
        # for that is to rebuild.
        if self.book.crossed:
            self._crossed_cycles += 1
            if self._crossed_cycles >= self.crossed_limit:
                self.book.resync()
                self._crossed_cycles = 0
        else:
            self._crossed_cycles = 0
            if self.book.seq != self._last_published_seq:
                self._last_published_seq = self.book.seq
                await self.hub.broadcast(self.book_frame())

                items, elided = self.book.take_trades(
                    self.max_trades_per_frame
                )
                if items:
                    await self.hub.broadcast(protocol.trades(items, elided))

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
            # Kept apart deliberately: ring_drops is the engine throwing
            # events away, reader_skipped is this server jumping forward to
            # stay current. Adding them together made the dashboard report
            # engine data loss that had not happened.
            ring_drops=self.reader.dropped,
            schedule_misses=snapshot.schedule_misses if snapshot else 0,
            reader_lag=self.reader.lag,
            reader_skipped=self.skipped,
            book_resyncs=self.book.resyncs,
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
        self._last_book: dict[str, Any] | None = None

    def book_frame(self) -> dict[str, Any] | None:
        return self._last_book

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
                self._last_book = frame
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
        backlog = 0 if args.replay_backlog else reader.seek_to_live()
        print(
            f"# attached to {args.shm}: capacity {reader.capacity} records, "
            f"producer {'live' if reader.producer_live else 'NOT running'}",
            file=sys.stderr,
        )
        if backlog:
            print(
                f"# started live, passing over {backlog} records already in "
                f"the ring (--replay-backlog to play them instead)",
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
        # The current book, so a client that connects while paused sees the
        # market rather than "waiting for a book frame" forever.
        current = publisher.book_frame()
        if current is not None:
            await ws.send(json.dumps(current))
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
    parser.add_argument("--replay-backlog", action="store_true",
                        help="play the records already in the ring at attach "
                             "time instead of starting from the live head")
    parser.add_argument("--build-sha", default="unknown")
    args = parser.parse_args(argv)

    try:
        return asyncio.run(serve(args))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
