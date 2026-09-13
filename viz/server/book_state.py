"""The server's own copy of the book, built by applying tap deltas.

**The server aggregates, not the engine.** The engine emits per-event records
and nothing else; turning tens of millions of events into a 30 Hz depth
snapshot is the server's job. Putting that work in the engine would mean the
matching core doing arithmetic on behalf of a dashboard, which is exactly
backwards.

This is an L2 view: price levels and aggregate quantities. The tap carries
per-order events, so it could be an L3 reconstruction, but nothing the UI
draws needs queue position and maintaining it would cost memory per resting
order in a Python process.

**A delta book cannot survive a gap.** Every level here exists because an Ack
was seen and no Cancelled or Fill has cancelled it out. Lose one Cancelled to
a full ring and that level rests forever: a phantom that never trades and
never leaves. Lose enough of them and the two sides walk through each other
and the dashboard shows a crossed book -- a book that the engine, which is
price-time priority and matches on arrival, could not possibly have held.

So the reader tells this class when it has skipped events, and the answer is
to throw the book away and rebuild it from what comes next. A partial book
that is converging is worth more than a complete-looking one that is wrong,
and the alternative -- patching around the gap -- is how market-data bugs
become permanent.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from .shm_reader import (
    EVENT_ACK,
    EVENT_CANCELLED,
    EVENT_FILL,
    EVENT_REPLACED,
    EVENT_TRADE,
    TapRecord,
)


@dataclass
class Level:
    qty: int = 0
    orders: int = 0


@dataclass
class BookState:
    """Applies engine events to a depth view."""

    bids: dict[int, Level] = field(default_factory=dict)
    asks: dict[int, Level] = field(default_factory=dict)
    # order id -> (side, price, open qty)
    orders: dict[int, tuple[int, int, int]] = field(default_factory=dict)
    seq: int = 0
    t_ns: int = 0
    last_trade: dict[str, int | str] | None = None
    pending_trades: list[dict[str, int | str]] = field(default_factory=list)
    events_applied: int = 0
    resyncs: int = 0

    def resync(self) -> None:
        """Discards the book after a gap in the event stream.

        Depth rebuilds from the orders acked after the gap, so it is thin for
        a moment and then correct. Sequence and time are kept: they describe
        where the stream is, not what the book holds.
        """
        self.bids.clear()
        self.asks.clear()
        self.orders.clear()
        self.resyncs += 1

    @property
    def crossed(self) -> bool:
        """True if the best bid is at or above the best ask.

        The engine cannot produce this -- an order that crossed would have
        traded -- so it is proof that this reconstruction has lost events.
        Checked rather than assumed, because the failure is silent otherwise:
        a crossed book still renders, it just renders a market that does not
        exist.
        """
        if not self.bids or not self.asks:
            return False
        return max(self.bids) >= min(self.asks)

    def _levels(self, side: int) -> dict[int, Level]:
        return self.bids if side == 0 else self.asks

    def apply(self, record: TapRecord) -> None:
        self.seq = record.seq
        self.t_ns = record.ts
        self.events_applied += 1

        if record.type == EVENT_ACK:
            # A market order's ack carries price 0 and never rests; the fills
            # that follow are what matter. kEventFlagMarketOrder is bit 0.
            if record.flags & 1:
                return
            self._add(record.id, record.side, record.price, record.qty)
        elif record.type == EVENT_TRADE:
            # The public print. The aggressor's own resting state is handled
            # by its Ack and by the Fill on the other side.
            self.pending_trades.append(
                {
                    "t_ns": record.ts,
                    "price": record.price,
                    "qty": record.qty,
                    "aggressor": record.side_char,
                    "resting_id": record.counter_id,
                }
            )
            self.last_trade = {
                "price": record.price,
                "qty": record.qty,
                "side": record.side_char,
            }
            # The aggressor may have been resting (a crossing replace); if so
            # it is leaving the book by the quantity it just traded.
            self._reduce(record.id, record.qty)
        elif record.type in (EVENT_FILL, EVENT_CANCELLED):
            # A fill is the resting side of a match; a cancellation is an
            # order leaving. Both take quantity off a level, and the engine's
            # Cancelled event carries what was still open, so the arithmetic
            # is the same.
            self._reduce(record.id, record.qty)
        elif record.type == EVENT_REPLACED:
            self._replace(record.id, record.price, record.qty)

    # -- book mutation ------------------------------------------------------

    def _add(self, order_id: int, side: int, price: int, qty: int) -> None:
        if qty <= 0:
            return
        self.orders[order_id] = (side, price, qty)
        level = self._levels(side).setdefault(price, Level())
        level.qty += qty
        level.orders += 1

    def _reduce(self, order_id: int, qty: int) -> None:
        entry = self.orders.get(order_id)
        if entry is None:
            return
        side, price, remaining = entry
        taken = min(qty, remaining)
        levels = self._levels(side)
        level = levels.get(price)
        if level is not None:
            level.qty -= taken
            if remaining - taken == 0:
                level.orders -= 1
            if level.orders <= 0 or level.qty <= 0:
                levels.pop(price, None)
        if remaining - taken == 0:
            self.orders.pop(order_id, None)
        else:
            self.orders[order_id] = (side, price, remaining - taken)

    def _replace(self, order_id: int, new_price: int, new_qty: int) -> None:
        entry = self.orders.get(order_id)
        if entry is None:
            return
        side, _price, remaining = entry
        self._reduce(order_id, remaining)
        self._add(order_id, side, new_price, new_qty)

    # -- views --------------------------------------------------------------

    def depth(self, levels: int) -> tuple[list[list[int]], list[list[int]]]:
        bids = sorted(self.bids.items(), reverse=True)[:levels]
        asks = sorted(self.asks.items())[:levels]
        return (
            [[price, lvl.qty, lvl.orders] for price, lvl in bids],
            [[price, lvl.qty, lvl.orders] for price, lvl in asks],
        )

    def take_trades(
        self, limit: int | None = None
    ) -> tuple[list[dict[str, int | str]], int]:
        """The trades since the last call, newest-last, and how many were
        dropped to stay under `limit`.

        The tape renders the most recent few hundred prints and nothing else,
        so sending every trade in a publish interval is work done to be thrown
        away: at 200k events/s one interval can hold thousands, and a frame of
        them crossed a megabyte on the wire. What is dropped is reported
        rather than quietly omitted -- the tape is a sample of the tape, and
        the panel says so.
        """
        out = self.pending_trades
        self.pending_trades = []
        if limit is None or len(out) <= limit:
            return out, 0
        return out[-limit:], len(out) - limit

    @property
    def orders_resting(self) -> int:
        return len(self.orders)
