"""Directed tests for the reference engine, one per rule in the semantics doc.

These pin the *oracle's* behaviour. They exist so that a disagreement with the
C++ engine can be attributed: if these pass and the differential test fails,
the C++ side is wrong; if these fail, the oracle is.
"""

from __future__ import annotations

import pytest

from pyref.command_io import CANCEL, NEW, REPLACE, Command
from pyref.reference_engine import Config, ReferenceEngine


def new(order_id, side, price, qty, tif="Day", order_type="Limit", participant=0, ts=0):
    return Command(
        verb=NEW,
        ts=ts,
        id=order_id,
        side=side,
        price=price,
        qty=qty,
        type=order_type,
        tif=tif,
        participant=participant,
    )


def cancel(order_id, ts=0):
    return Command(verb=CANCEL, ts=ts, id=order_id)


def replace(order_id, price, qty, ts=0):
    return Command(verb=REPLACE, ts=ts, id=order_id, price=price, qty=qty)


def run(*commands, stp=False):
    engine = ReferenceEngine(Config(self_trade_prevention=stp))
    engine.apply_all(list(commands))
    return engine


def events_of(engine, event_type):
    return [e for e in engine.events if e.type == event_type]


def queue_at(engine, side, price):
    levels = engine.book.bids if side == "B" else engine.book.asks
    return [o.id for o in levels.get(price, [])]


# --- 1. price-time priority -------------------------------------------------


def test_better_price_executes_first():
    engine = run(
        new(1, "S", 1002400, 100),
        new(2, "S", 1002200, 100),
        new(3, "S", 1002300, 100),
        new(4, "B", 1002400, 300),
    )
    prices = [e.price for e in events_of(engine, "TRADE")]
    assert prices == [1002200, 1002300, 1002400]


def test_fifo_within_a_level():
    engine = run(
        new(1, "S", 1002200, 100),
        new(2, "S", 1002200, 100),
        new(3, "B", 1002200, 100),
    )
    trades = events_of(engine, "TRADE")
    assert len(trades) == 1
    assert trades[0].counter_id == 1


def test_aggressor_stops_at_its_limit():
    engine = run(
        new(1, "S", 1002200, 100),
        new(2, "S", 1002300, 100),
        new(3, "S", 1002400, 100),
        new(4, "B", 1002300, 300),
    )
    assert len(events_of(engine, "TRADE")) == 2
    assert engine.book.best("B") == 1002300
    assert engine.book.qty_at("B", 1002300) == 100


# --- 2. trade price ---------------------------------------------------------


def test_trade_prints_at_the_resting_price():
    engine = run(new(1, "S", 1000000, 100), new(2, "B", 1010000, 100))
    trades = events_of(engine, "TRADE")
    assert trades[0].price == 1000000


def test_trade_prints_at_the_resting_price_when_the_bid_rests():
    engine = run(new(1, "B", 1010000, 100), new(2, "S", 1000000, 100))
    assert events_of(engine, "TRADE")[0].price == 1010000


# --- 3. order types and time in force ---------------------------------------


def test_limit_day_rests_the_residual():
    engine = run(new(1, "S", 1002200, 40), new(2, "B", 1002200, 100))
    assert engine.book.qty_at("B", 1002200) == 60
    assert not events_of(engine, "CANCELLED")


def test_limit_ioc_cancels_the_residual():
    engine = run(new(1, "S", 1002200, 40), new(2, "B", 1002200, 100, tif="IOC"))
    assert engine.book.qty_at("B", 1002200) == 0
    cancels = events_of(engine, "CANCELLED")
    assert len(cancels) == 1 and cancels[0].qty == 60


def test_limit_fok_executes_nothing_when_it_cannot_fill():
    engine = run(new(1, "S", 1002200, 40), new(2, "B", 1002200, 100, tif="FOK"))
    assert not events_of(engine, "TRADE")
    assert engine.book.qty_at("S", 1002200) == 40
    assert events_of(engine, "REJECT")[0].reason == "FokUnfillable"


def test_limit_fok_fills_across_levels_when_it_can():
    engine = run(
        new(1, "S", 1002200, 40),
        new(2, "S", 1002300, 40),
        new(3, "S", 1002400, 40),
        new(4, "B", 1002400, 100, tif="FOK"),
    )
    assert sum(e.qty for e in events_of(engine, "TRADE")) == 100
    assert not events_of(engine, "REJECT")


def test_post_only_rejects_rather_than_sliding():
    engine = run(
        new(1, "S", 1002400, 100), new(2, "B", 1002400, 100, tif="PostOnly")
    )
    assert not events_of(engine, "TRADE")
    assert engine.book.order_count() == 1
    assert events_of(engine, "REJECT")[0].reason == "PostOnlyWouldCross"


def test_post_only_rests_when_it_would_not_cross():
    engine = run(
        new(1, "S", 1002400, 100), new(2, "B", 1002300, 100, tif="PostOnly")
    )
    assert engine.book.qty_at("B", 1002300) == 100
    assert not events_of(engine, "REJECT")


def test_market_ioc_is_a_limit_at_infinity():
    engine = run(
        new(1, "S", 1002200, 40),
        new(2, "S", 9999999, 40),
        new(3, "B", 0, 80, tif="IOC", order_type="Market"),
    )
    assert [e.price for e in events_of(engine, "TRADE")] == [1002200, 9999999]
    assert engine.book.order_count() == 0


def test_market_day_is_rejected():
    engine = run(new(1, "B", 0, 100, order_type="Market"))
    assert events_of(engine, "REJECT")[0].reason == "MarketDayNotSupported"
    assert not events_of(engine, "ACK")


def test_market_fok_rejects_when_it_cannot_fill_entirely():
    engine = run(
        new(1, "S", 1002200, 40),
        new(2, "B", 0, 100, tif="FOK", order_type="Market"),
    )
    assert events_of(engine, "REJECT")[0].reason == "MarketOrderNotFillable"


# --- 4. cancel --------------------------------------------------------------


def test_cancel_reports_only_the_open_quantity():
    engine = run(
        new(1, "B", 1002100, 100), new(2, "S", 1002100, 30), cancel(1)
    )
    assert events_of(engine, "CANCELLED")[-1].qty == 70


def test_cancel_of_unknown_and_of_completed_differ():
    engine = run(
        new(1, "B", 1002100, 100),
        new(2, "S", 1002100, 100),
        cancel(1),
        cancel(999),
    )
    reasons = [e.reason for e in events_of(engine, "REJECT")]
    assert reasons == ["TooLateToCancel", "UnknownOrder"]


def test_completed_id_memory_is_bounded():
    engine = ReferenceEngine(Config(completed_capacity=2))
    for order_id in (1, 2, 3):
        engine.apply(new(order_id, "B", 1002100, 10))
        engine.apply(cancel(order_id))
    engine.apply(cancel(3))
    engine.apply(cancel(1))
    reasons = [e.reason for e in events_of(engine, "REJECT")]
    assert reasons == ["TooLateToCancel", "UnknownOrder"]


# --- 5. replace -------------------------------------------------------------


def test_size_down_keeps_priority():
    engine = run(
        new(1, "B", 1002100, 100),
        new(2, "B", 1002100, 100),
        new(3, "B", 1002100, 100),
        replace(2, 1002100, 50),
    )
    assert queue_at(engine, "B", 1002100) == [1, 2, 3]
    assert engine.book.qty_at("B", 1002100) == 250


def test_size_up_loses_priority():
    engine = run(
        new(1, "B", 1002100, 100),
        new(2, "B", 1002100, 100),
        new(3, "B", 1002100, 100),
        replace(1, 1002100, 200),
    )
    assert queue_at(engine, "B", 1002100) == [2, 3, 1]


def test_price_change_loses_priority_in_both_directions():
    engine = run(
        new(1, "B", 1002100, 100),
        new(2, "B", 1002200, 100),
        new(3, "B", 1002200, 100),
        replace(1, 1002200, 100),
    )
    assert queue_at(engine, "B", 1002200) == [2, 3, 1]

    engine = run(
        new(1, "B", 1002200, 100),
        new(2, "B", 1002100, 100),
        replace(1, 1002100, 100),
    )
    assert queue_at(engine, "B", 1002100) == [2, 1]


def test_crossing_replace_matches_immediately_after_the_replaced_event():
    engine = run(
        new(1, "S", 1002300, 60),
        new(2, "B", 1002100, 100),
        replace(2, 1002300, 100),
    )
    # The priority change is reported before the trades it causes.
    assert [e.type for e in engine.events] == [
        "ACK",
        "ACK",
        "REPLACED",
        "TRADE",
        "FILL",
    ]
    trades = events_of(engine, "TRADE")
    assert trades[0].price == 1002300 and trades[0].qty == 60
    assert engine.book.qty_at("B", 1002300) == 40


def test_replace_of_unknown_or_completed_is_rejected():
    engine = run(replace(42, 1002100, 100))
    assert events_of(engine, "REJECT")[0].reason == "UnknownOrder"

    engine = run(
        new(1, "B", 1002100, 100),
        new(2, "S", 1002100, 100),
        replace(1, 1002200, 50),
    )
    assert events_of(engine, "REJECT")[0].reason == "TooLateToCancel"


def test_replace_to_zero_quantity_leaves_the_order_alone():
    engine = run(new(1, "B", 1002100, 100), replace(1, 1002100, 0))
    assert events_of(engine, "REJECT")[0].reason == "ZeroQuantity"
    assert engine.book.qty_at("B", 1002100) == 100


# --- 6. self-trade prevention -----------------------------------------------


def test_stp_is_off_by_default():
    engine = run(
        new(1, "S", 1002200, 100, participant=7),
        new(2, "B", 1002200, 100, participant=7),
    )
    assert len(events_of(engine, "TRADE")) == 1


def test_stp_cancels_the_resting_order_and_the_aggressor_walks_on():
    engine = run(
        new(1, "S", 1002200, 100, participant=7),
        new(2, "S", 1002300, 100, participant=9),
        new(3, "B", 1002300, 100, participant=7),
        stp=True,
    )
    trades = events_of(engine, "TRADE")
    assert len(trades) == 1 and trades[0].counter_id == 2
    cancels = events_of(engine, "CANCELLED")
    assert cancels[0].id == 1 and cancels[0].reason == "SelfTradePrevented"


def test_participant_zero_never_self_trade_prevents():
    engine = run(new(1, "S", 1002200, 100), new(2, "B", 1002200, 100), stp=True)
    assert len(events_of(engine, "TRADE")) == 1
    assert engine.stats.stp_cancels == 0


def test_fok_prescan_excludes_liquidity_stp_will_cancel():
    engine = run(
        new(1, "S", 1002200, 100, participant=7),
        new(2, "B", 1002200, 100, tif="FOK", participant=7),
        stp=True,
    )
    assert not events_of(engine, "TRADE")
    assert events_of(engine, "REJECT")[0].reason == "FokUnfillable"
    assert engine.book.qty_at("S", 1002200) == 100


# --- 7. rejects -------------------------------------------------------------


@pytest.mark.parametrize(
    ("command", "reason"),
    [
        (new(1, "B", 1002100, 0), "ZeroQuantity"),
        (new(1, "B", 0, 100), "InvalidPrice"),
        (new(1, "B", -5, 100), "InvalidPrice"),
    ],
)
def test_reject_reasons(command, reason):
    engine = run(command)
    assert events_of(engine, "REJECT")[0].reason == reason


def test_duplicate_live_id_is_rejected():
    engine = run(new(1, "B", 1002100, 100), new(1, "B", 1002100, 100))
    assert events_of(engine, "REJECT")[0].reason == "DuplicateOrderId"
    assert engine.book.qty_at("B", 1002100) == 100


def test_price_band():
    engine = ReferenceEngine(Config(min_price=1000000, max_price=1100000))
    engine.apply(new(1, "B", 900000, 10))
    engine.apply(new(2, "B", 1200000, 10))
    engine.apply(new(3, "B", 1050000, 10))
    reasons = [e.reason for e in events_of(engine, "REJECT")]
    assert reasons == ["PriceOutOfBand", "PriceOutOfBand"]
    assert engine.book.order_count() == 1


# --- 9. sequence numbering --------------------------------------------------


def test_sequence_numbers_are_contiguous():
    engine = run(
        new(1, "S", 1002200, 100),
        new(2, "B", 1002200, 60),
        cancel(1),
        cancel(1),
    )
    assert [e.seq for e in engine.events] == list(range(len(engine.events)))
