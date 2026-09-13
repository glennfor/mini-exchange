"""The reference order book: dictionaries and lists, no cleverness.

Storage and ordering only. Matching policy lives in ``reference_engine``,
mirroring the split described in ``docs/architecture.md`` -- not because this
implementation needs the separation for performance, but because keeping the
two oracles structurally comparable makes a disagreement easier to localise.
"""

from __future__ import annotations

from dataclasses import dataclass, field

BUY = "B"
SELL = "S"

INVALID_PRICE = -(2**63)
MARKET_BUY_PRICE = 2**63 - 1
MARKET_SELL_PRICE = -(2**63) + 1


def opposite(side: str) -> str:
    return SELL if side == BUY else BUY


def better_price(side: str, a: int, b: int) -> bool:
    """True if ``a`` is a strictly more aggressive price than ``b``."""
    return a > b if side == BUY else a < b


def crosses(side: str, limit: int, resting: int) -> bool:
    """True if an order on ``side`` with ``limit`` will trade at ``resting``."""
    return limit >= resting if side == BUY else limit <= resting


@dataclass
class Order:
    id: int
    side: str
    price: int
    qty_open: int
    qty_orig: int
    ts_entry: int
    participant: int
    seq_entry: int  # engine acceptance order; the FIFO key


@dataclass
class ReferenceBook:
    # price -> orders at that price, oldest first.
    bids: dict[int, list[Order]] = field(default_factory=dict)
    asks: dict[int, list[Order]] = field(default_factory=dict)
    index: dict[int, Order] = field(default_factory=dict)

    # -- helpers ------------------------------------------------------------

    def _levels(self, side: str) -> dict[int, list[Order]]:
        return self.bids if side == BUY else self.asks

    def prices(self, side: str) -> list[int]:
        """Every occupied price on ``side``, ordered from the touch outwards."""
        return sorted(self._levels(side).keys(), reverse=(side == BUY))

    # -- mutation -----------------------------------------------------------

    def add(self, order: Order) -> bool:
        if order.id in self.index:
            return False
        self._levels(order.side).setdefault(order.price, []).append(order)
        self.index[order.id] = order
        return True

    def cancel(self, order_id: int) -> bool:
        order = self.index.pop(order_id, None)
        if order is None:
            return False
        levels = self._levels(order.side)
        queue = levels[order.price]
        queue.remove(order)
        if not queue:
            del levels[order.price]
        return True

    def reduce(self, order_id: int, by: int) -> bool:
        order = self.index.get(order_id)
        if order is None or by > order.qty_open:
            return False
        order.qty_open -= by
        if order.qty_open == 0:
            self.cancel(order_id)
        return True

    def fill(self, order: Order, qty: int) -> None:
        order.qty_open -= qty
        if order.qty_open == 0:
            self.cancel(order.id)

    # -- traversal ----------------------------------------------------------

    def best(self, side: str) -> int:
        prices = self.prices(side)
        return prices[0] if prices else INVALID_PRICE

    def front(self, side: str) -> Order | None:
        prices = self.prices(side)
        if not prices:
            return None
        return self._levels(side)[prices[0]][0]

    def qty_at(self, side: str, price: int, exclude_participant: int = 0) -> int:
        queue = self._levels(side).get(price, [])
        if exclude_participant == 0:
            return sum(o.qty_open for o in queue)
        return sum(o.qty_open for o in queue if o.participant != exclude_participant)

    def find(self, order_id: int) -> Order | None:
        return self.index.get(order_id)

    def order_count(self) -> int:
        return len(self.index)
