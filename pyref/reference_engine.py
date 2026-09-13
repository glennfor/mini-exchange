"""The reference matching engine.

Written from ``docs/matching-semantics.md``. Section numbers in the comments
below refer to that document; if this file and the C++ engine disagree, the
document decides which one is wrong.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field

from .command_io import CANCEL, NEW, REPLACE, Command, Event
from .reference_book import (
    BUY,
    INVALID_PRICE,
    MARKET_BUY_PRICE,
    MARKET_SELL_PRICE,
    Order,
    ReferenceBook,
    crosses,
    opposite,
)

# Reject reasons, spelled exactly as the canonical event format spells them.
NONE = "None"
DUPLICATE_ORDER_ID = "DuplicateOrderId"
UNKNOWN_ORDER = "UnknownOrder"
TOO_LATE_TO_CANCEL = "TooLateToCancel"
ZERO_QUANTITY = "ZeroQuantity"
INVALID_PRICE_REASON = "InvalidPrice"
PRICE_OUT_OF_BAND = "PriceOutOfBand"
POOL_EXHAUSTED = "PoolExhausted"
FOK_UNFILLABLE = "FokUnfillable"
POST_ONLY_WOULD_CROSS = "PostOnlyWouldCross"
MARKET_ORDER_NOT_FILLABLE = "MarketOrderNotFillable"
MARKET_DAY_NOT_SUPPORTED = "MarketDayNotSupported"
SELF_TRADE_PREVENTED = "SelfTradePrevented"

DEFAULT_COMPLETED_CAPACITY = 65536


@dataclass
class Config:
    symbol: int = 0
    self_trade_prevention: bool = False
    min_price: int = 1
    max_price: int = 0  # 0 disables the upper bound
    max_fok_scan_levels: int = 4096
    completed_capacity: int = DEFAULT_COMPLETED_CAPACITY


class CompletedIds:
    """Bounded FIFO memory of completed order ids (semantics document, 4).

    The bound is what makes ``TooLateToCancel`` answerable without unbounded
    memory, and the boundary behaviour -- an id that fell out of the window
    reports ``UnknownOrder`` -- is part of the contract, so the capacity here
    must match the C++ side's.
    """

    def __init__(self, capacity: int) -> None:
        self.capacity = capacity
        self._order: deque[int] = deque()
        self._set: set[int] = set()

    def insert(self, order_id: int) -> None:
        if self.capacity == 0 or order_id in self._set:
            return
        if len(self._order) == self.capacity:
            self._set.discard(self._order.popleft())
        self._order.append(order_id)
        self._set.add(order_id)

    def __contains__(self, order_id: int) -> bool:
        return order_id in self._set


@dataclass
class Stats:
    commands: int = 0
    rejects: int = 0
    trades: int = 0
    shares_traded: int = 0
    stp_cancels: int = 0


@dataclass
class ReferenceEngine:
    config: Config = field(default_factory=Config)
    book: ReferenceBook = field(default_factory=ReferenceBook)
    events: list[Event] = field(default_factory=list)
    stats: Stats = field(default_factory=Stats)

    def __post_init__(self) -> None:
        self._seq = 0
        self._match_id = 0
        self._entry = 0
        self._completed = CompletedIds(self.config.completed_capacity)
        # Only needed so the ledger-style checks in the tests can tell a
        # market order's Ack from a limit order priced at zero.
        self._market_orders: set[int] = set()

    # -- dispatch -----------------------------------------------------------

    def apply(self, cmd: Command) -> None:
        self.stats.commands += 1
        if cmd.verb == NEW:
            self.on_new(cmd)
        elif cmd.verb == CANCEL:
            self.on_cancel(cmd)
        elif cmd.verb == REPLACE:
            self.on_replace(cmd)

    def apply_all(self, commands: list[Command]) -> None:
        for cmd in commands:
            self.apply(cmd)

    def event_lines(self) -> list[str]:
        return [e.to_line() for e in self.events]

    # -- new order ----------------------------------------------------------

    def on_new(self, cmd: Command) -> None:
        if cmd.qty == 0:
            return self._reject(cmd.id, ZERO_QUANTITY)

        # Semantics 3: a market order that may not cross, or that would have
        # to rest, can never do anything.
        if cmd.type == "Market" and cmd.tif in ("Day", "PostOnly"):
            return self._reject(cmd.id, MARKET_DAY_NOT_SUPPORTED)

        if cmd.type == "Market":
            limit = MARKET_BUY_PRICE if cmd.side == BUY else MARKET_SELL_PRICE
        else:
            bad = self._check_price(cmd.price)
            if bad is not None:
                return self._reject(cmd.id, bad)
            limit = cmd.price

        if self.book.find(cmd.id) is not None:
            return self._reject(cmd.id, DUPLICATE_ORDER_ID)

        # Semantics 3: PostOnly rejects outright rather than sliding.
        if cmd.tif == "PostOnly":
            opp = self.book.best(opposite(cmd.side))
            if opp != INVALID_PRICE and crosses(cmd.side, limit, opp):
                return self._reject(cmd.id, POST_ONLY_WOULD_CROSS)

        # Semantics 3: the fill-or-kill pre-scan, non-mutating.
        if (
            cmd.tif == "FOK"
            and self.fillable_qty(cmd.side, limit, cmd.qty, cmd.participant) < cmd.qty
        ):
            reason = (
                MARKET_ORDER_NOT_FILLABLE if cmd.type == "Market" else FOK_UNFILLABLE
            )
            return self._reject(cmd.id, reason)

        ack_price = 0 if cmd.type == "Market" else cmd.price
        if cmd.type == "Market":
            self._market_orders.add(cmd.id)
        self._emit(
            Event(
                seq=self._next_seq(),
                type="ACK",
                id=cmd.id,
                side=cmd.side,
                price=ack_price,
                qty=cmd.qty,
                qty_remaining=cmd.qty,
                is_market=cmd.type == "Market",
            )
        )
        self._accept(
            cmd.id,
            cmd.side,
            limit,
            cmd.qty,
            cmd.type,
            cmd.tif,
            cmd.participant,
            cmd.ts,
        )

    # -- cancel -------------------------------------------------------------

    def on_cancel(self, cmd: Command) -> None:
        order = self.book.find(cmd.id)
        if order is None:
            return self._reject(
                cmd.id,
                TOO_LATE_TO_CANCEL if cmd.id in self._completed else UNKNOWN_ORDER,
            )
        open_qty = order.qty_open
        side = order.side
        price = order.price
        self.book.cancel(cmd.id)
        self._completed.insert(cmd.id)
        self._emit(
            Event(
                seq=self._next_seq(),
                type="CANCELLED",
                id=cmd.id,
                side=side,
                price=price,
                qty=open_qty,
                qty_remaining=0,
            )
        )

    # -- replace ------------------------------------------------------------

    def on_replace(self, cmd: Command) -> None:
        order = self.book.find(cmd.id)
        if order is None:
            return self._reject(
                cmd.id,
                TOO_LATE_TO_CANCEL if cmd.id in self._completed else UNKNOWN_ORDER,
            )
        if cmd.qty == 0:
            return self._reject(cmd.id, ZERO_QUANTITY)
        bad = self._check_price(cmd.price)
        if bad is not None:
            return self._reject(cmd.id, bad)

        side = order.side
        old_price = order.price
        open_qty = order.qty_open
        participant = order.participant

        # Semantics 5, row 1: a size-down at the same price keeps priority.
        if cmd.price == old_price and cmd.qty <= open_qty:
            if cmd.qty < open_qty:
                self.book.reduce(cmd.id, open_qty - cmd.qty)
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
            return

        # Everything else loses priority. The reference book never runs out of
        # storage, so the PoolExhausted path cannot be reached here; it exists
        # in the C++ engine and is exercised there with an instrumented book.
        self.book.cancel(cmd.id)
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
            cmd.id, side, cmd.price, cmd.qty, "Limit", "Day", participant, cmd.ts
        )

    # -- the single accept/match path ---------------------------------------

    def _accept(
        self,
        order_id: int,
        side: str,
        limit: int,
        qty: int,
        order_type: str,
        tif: str,
        participant: int,
        ts: int,
    ) -> None:
        remaining = self._match(order_id, side, limit, qty, participant)
        if remaining == 0:
            self._completed.insert(order_id)
            return

        may_rest = order_type == "Limit" and tif in ("Day", "PostOnly")
        if may_rest:
            self._entry += 1
            self.book.add(
                Order(
                    id=order_id,
                    side=side,
                    price=limit,
                    qty_open=remaining,
                    qty_orig=qty,
                    ts_entry=ts,
                    participant=participant,
                    seq_entry=self._entry,
                )
            )
            return

        self._completed.insert(order_id)
        self._emit(
            Event(
                seq=self._next_seq(),
                type="CANCELLED",
                id=order_id,
                side=side,
                price=limit,
                qty=remaining,
                qty_remaining=0,
            )
        )

    def _match(
        self, order_id: int, side: str, limit: int, qty: int, participant: int
    ) -> int:
        remaining = qty
        other = opposite(side)

        while remaining > 0:
            resting = self.book.front(other)
            if resting is None:
                break
            if not crosses(side, limit, resting.price):
                break

            # Semantics 6: STP cancels the resting order; the aggressor walks on.
            if self._stp_active(participant) and resting.participant == participant:
                self.book.cancel(resting.id)
                self._completed.insert(resting.id)
                self.stats.stp_cancels += 1
                self._emit(
                    Event(
                        seq=self._next_seq(),
                        type="CANCELLED",
                        id=resting.id,
                        side=other,
                        price=resting.price,
                        qty=resting.qty_open,
                        qty_remaining=0,
                        reason=SELF_TRADE_PREVENTED,
                    )
                )
                continue

            # Semantics 2: the trade prints at the RESTING order's price.
            trade_price = resting.price
            fill = min(remaining, resting.qty_open)
            resting_after = resting.qty_open - fill

            self._match_id += 1
            self.stats.trades += 1
            self.stats.shares_traded += fill
            remaining -= fill

            self._emit(
                Event(
                    seq=self._next_seq(),
                    type="TRADE",
                    id=order_id,
                    counter_id=resting.id,
                    match_id=self._match_id,
                    side=side,
                    price=trade_price,
                    qty=fill,
                    qty_remaining=remaining,
                )
            )
            self._emit(
                Event(
                    seq=self._next_seq(),
                    type="FILL",
                    id=resting.id,
                    counter_id=order_id,
                    match_id=self._match_id,
                    side=other,
                    price=trade_price,
                    qty=fill,
                    qty_remaining=resting_after,
                )
            )

            self.book.fill(resting, fill)
            if resting_after == 0:
                self._completed.insert(resting.id)

        return remaining

    # -- helpers ------------------------------------------------------------

    def fillable_qty(self, side: str, limit: int, want: int, participant: int) -> int:
        """Non-mutating: how much this order could execute right now."""
        total = 0
        other = opposite(side)
        exclude = participant if self._stp_active(participant) else 0
        for levels, price in enumerate(self.book.prices(other)):
            if not crosses(side, limit, price):
                break
            if levels >= self.config.max_fok_scan_levels:
                break
            total += self.book.qty_at(other, price, exclude_participant=exclude)
            if total >= want:
                return want
        return total

    def _stp_active(self, participant: int) -> bool:
        return self.config.self_trade_prevention and participant != 0

    def _check_price(self, price: int) -> str | None:
        if price <= 0 or price in (MARKET_BUY_PRICE, MARKET_SELL_PRICE):
            return INVALID_PRICE_REASON
        if price < self.config.min_price:
            return PRICE_OUT_OF_BAND
        if self.config.max_price != 0 and price > self.config.max_price:
            return PRICE_OUT_OF_BAND
        return None

    def _reject(self, order_id: int, reason: str) -> None:
        self.stats.rejects += 1
        self._emit(
            Event(seq=self._next_seq(), type="REJECT", id=order_id, reason=reason)
        )

    def _next_seq(self) -> int:
        seq = self._seq
        self._seq += 1
        return seq

    def _emit(self, event: Event) -> None:
        self.events.append(event)
