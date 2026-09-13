"""The server's book, built from engine events.

The server aggregates, not the engine: turning tens of millions of per-event
records into a 30 Hz depth snapshot is the server's job, and these tests pin
down that it does the arithmetic right.
"""

from __future__ import annotations

from viz.server.book_state import BookState
from viz.server.shm_reader import (
    EVENT_ACK,
    EVENT_CANCELLED,
    EVENT_FILL,
    EVENT_REPLACED,
    EVENT_TRADE,
    TapRecord,
)


def rec(seq, etype, order_id=0, price=0, qty=0, side=0, counter_id=0,
        remaining=0, flags=0):
    return TapRecord(
        seq=seq, ts=seq * 1000, price=price, qty=qty, qty_remaining=remaining,
        id=order_id, counter_id=counter_id, level_index=0, type=etype,
        side=side, reason=0, flags=flags,
    )


def test_acks_build_levels():
    book = BookState()
    book.apply(rec(0, EVENT_ACK, 1, 1002100, 500, side=0))
    book.apply(rec(1, EVENT_ACK, 2, 1002100, 300, side=0))
    book.apply(rec(2, EVENT_ACK, 3, 1002400, 200, side=1))

    bids, asks = book.depth(10)
    assert bids == [[1002100, 800, 2]]
    assert asks == [[1002400, 200, 1]]
    assert book.orders_resting == 3


def test_a_market_orders_ack_never_rests():
    # A market order's Ack carries price 0 and the market flag. Adding it to
    # the book would put a phantom level at price zero at the top of the bid
    # side, which is about as wrong as a depth ladder can look.
    book = BookState()
    book.apply(rec(0, EVENT_ACK, 1, 0, 500, side=0, flags=1))
    bids, _ = book.depth(10)
    assert bids == []
    assert book.orders_resting == 0


def test_fills_reduce_the_resting_side():
    book = BookState()
    book.apply(rec(0, EVENT_ACK, 1, 1002400, 500, side=1))
    book.apply(rec(1, EVENT_FILL, 1, 1002400, 200, side=1))
    _, asks = book.depth(10)
    assert asks == [[1002400, 300, 1]]

    book.apply(rec(2, EVENT_FILL, 1, 1002400, 300, side=1))
    _, asks = book.depth(10)
    assert asks == []
    assert book.orders_resting == 0


def test_cancellations_remove_what_was_open():
    book = BookState()
    book.apply(rec(0, EVENT_ACK, 1, 1002100, 500, side=0))
    book.apply(rec(1, EVENT_CANCELLED, 1, 1002100, 500, side=0))
    assert book.depth(10)[0] == []


def test_trades_are_batched_for_the_tape_and_set_the_last_trade():
    book = BookState()
    book.apply(rec(0, EVENT_ACK, 1, 1002400, 500, side=1))
    book.apply(rec(1, EVENT_TRADE, 2, 1002400, 100, side=0, counter_id=1))

    trades = book.take_trades()
    assert len(trades) == 1
    assert trades[0]["price"] == 1002400
    assert trades[0]["aggressor"] == "B"
    assert trades[0]["resting_id"] == 1
    assert book.last_trade == {"price": 1002400, "qty": 100, "side": "B"}
    assert book.take_trades() == [], "taking must drain the batch"


def test_replace_moves_quantity_between_levels():
    book = BookState()
    book.apply(rec(0, EVENT_ACK, 1, 1002100, 500, side=0))
    book.apply(rec(1, EVENT_REPLACED, 1, 1002200, 300, side=0))
    bids, _ = book.depth(10)
    assert bids == [[1002200, 300, 1]]


def test_depth_is_ordered_from_the_touch_outwards():
    book = BookState()
    for i, price in enumerate([1002100, 1002000, 1002200]):
        book.apply(rec(i, EVENT_ACK, i + 1, price, 100, side=0))
    for i, price in enumerate([1002500, 1002400, 1002600]):
        book.apply(rec(10 + i, EVENT_ACK, 100 + i, price, 100, side=1))

    bids, asks = book.depth(10)
    assert [level[0] for level in bids] == [1002200, 1002100, 1002000]
    assert [level[0] for level in asks] == [1002400, 1002500, 1002600]


def test_events_for_unknown_orders_are_ignored_not_fatal():
    # The server may attach to a running engine and see fills for orders whose
    # acks it never saw. That is normal; it must not crash and must not
    # invent negative quantities.
    book = BookState()
    book.apply(rec(0, EVENT_FILL, 999, 1002100, 100, side=0))
    book.apply(rec(1, EVENT_CANCELLED, 999, 1002100, 100, side=0))
    assert book.depth(10) == ([], [])
    assert book.orders_resting == 0


def test_a_level_never_goes_negative():
    book = BookState()
    book.apply(rec(0, EVENT_ACK, 1, 1002100, 100, side=0))
    book.apply(rec(1, EVENT_FILL, 1, 1002100, 5000, side=0))
    bids, _ = book.depth(10)
    assert bids == []
    assert book.orders_resting == 0
