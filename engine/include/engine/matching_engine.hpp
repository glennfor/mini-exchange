/// \file matching_engine.hpp
/// \brief Price-time priority matching. The behavioural contract is
///        docs/matching-semantics.md; this file implements it and nothing
///        else.
///
/// Properties this code is required to have, and which the tests enforce:
///   * no allocation, no exceptions, no virtual dispatch, no syscalls, no
///     logging, and no clock reads on the hot path;
///   * every outbound event carries a monotonic `SeqNum`, and the whole
///     stream is a pure function of the command stream and the build;
///   * every reject maps to exactly one `RejectReason`.
///
/// The engine is templated on the book and the sink so that everything
/// inlines. It is single-threaded by design; see docs/design-decisions.md.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "core/config.hpp"
#include "core/spsc_ring.hpp"
#include "core/types.hpp"

#include "book/book_interface.hpp"
#include "engine/completed_ids.hpp"
#include "engine/messages.hpp"

namespace miniex {

/// Self-trade prevention policy.
///
/// Only `CancelResting` is implemented, behind a runtime flag, default off.
/// The alternatives -- cancel the incoming order, cancel both, or decrement
/// the larger and cancel the smaller -- are described in
/// docs/matching-semantics.md. Knowing why you picked one is the point;
/// implementing all four is not.
enum class SelfTradePolicy : std::uint8_t {
  None = 0,
  /// When an aggressor would match a resting order from the same
  /// participant, the *resting* order is cancelled and the aggressor keeps
  /// walking the book.
  CancelResting = 1,
};

struct EngineConfig {
  /// The single symbol this engine instance trades. Stamped onto every
  /// outbound event.
  SymbolId symbol{0};

  SelfTradePolicy stp{SelfTradePolicy::None};

  /// Inclusive price band for limit orders, in ticks. `max_price == 0`
  /// disables the upper bound. Prices at or below zero are always invalid.
  Price min_price{1};
  Price max_price{0};

  /// Upper bound on the number of price levels the fill-or-kill pre-scan
  /// will walk before giving up and rejecting. Without it, a FOK market
  /// order against a pathologically deep book is an unbounded loop on the
  /// hot path; with it the worst case is a fixed cost and a reject.
  std::size_t max_fok_scan_levels{4096};

  /// Size of the bounded recently-completed id memory. See
  /// engine/completed_ids.hpp for what the boundary means.
  std::size_t completed_capacity{kCompletedRingCapacity};
};

/// Counters the harness reports alongside throughput. Cheap enough to leave
/// on: they are increments of members already in cache.
struct EngineStats {
  std::uint64_t commands{0};
  std::uint64_t news{0};
  std::uint64_t cancels{0};
  std::uint64_t replaces{0};
  std::uint64_t rejects{0};
  std::uint64_t trades{0};
  std::uint64_t shares_traded{0};
  std::uint64_t stp_cancels{0};
};

template <OrderBook Book, class Sink>
class MatchingEngine {
public:
  /// Any trailing arguments are forwarded to the book's constructor, so a
  /// book that needs sizing (FastBook) can be given it without the engine
  /// knowing anything about what the sizing means.
  template <class... BookArgs>
  explicit MatchingEngine(Sink& sink, EngineConfig cfg = {},
                          BookArgs&&... book_args)
      : book_(std::forward<BookArgs>(book_args)...), sink_(sink), cfg_(cfg),
        completed_(cfg.completed_capacity) {}

  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine& operator=(const MatchingEngine&) = delete;

  // -------------------------------------------------------------------------
  // Command entry points
  // -------------------------------------------------------------------------

  void on_new(const NewOrder& n, TsNanos ts) noexcept {
    ++stats_.commands;
    ++stats_.news;

    if (n.qty == 0) return reject(n.id, ts, RejectReason::ZeroQuantity);

    // A market order that cannot fill has nowhere to go: resting it would
    // mean resting an order with no price, which is not a thing any exchange
    // supports. Rejected explicitly rather than silently converted.
    if (n.type == OrderType::Market && n.tif == TimeInForce::Day)
      return reject(n.id, ts, RejectReason::MarketDayNotSupported);
    if (n.type == OrderType::Market && n.tif == TimeInForce::PostOnly)
      return reject(n.id, ts, RejectReason::MarketDayNotSupported);

    Price limit = n.price;
    if (n.type == OrderType::Market) {
      limit = n.side == Side::Buy ? kMarketBuyPrice : kMarketSellPrice;
    } else {
      const RejectReason bad = check_price(n.price);
      if (bad != RejectReason::None) return reject(n.id, ts, bad);
    }

    if (book_.find(n.id) != nullptr)
      return reject(n.id, ts, RejectReason::DuplicateOrderId);

    // PostOnly: reject outright if it would cross. Deliberately *not* a
    // price slide -- a slide silently gives the client a different order
    // than the one they sent, and reconciling that is their problem forever.
    if (n.tif == TimeInForce::PostOnly) {
      const Price opp = book_.best(opposite(n.side));
      if (opp != kInvalidPrice && crosses(n.side, limit, opp))
        return reject(n.id, ts, RejectReason::PostOnlyWouldCross);
    }

    // Fill-or-kill: a non-mutating pre-scan, the one place where matching is
    // two-pass. Separated out so it is independently testable.
    if (n.tif == TimeInForce::FOK) {
      if (!can_fill_entirely(n.side, limit, n.qty, n.participant))
        return reject(n.id, ts,
                      n.type == OrderType::Market
                          ? RejectReason::MarketOrderNotFillable
                          : RejectReason::FokUnfillable);
    }

    // A market order has no price of its own; reporting 0 rather than the
    // ignored `price` field keeps the event stream a function of what the
    // order actually was.
    const Price ack_price = n.type == OrderType::Market ? 0 : n.price;
    Event ack = make_event(EventType::Ack, seq_++, ts, n.id, ack_price, n.qty,
                           n.qty, n.side, n.participant);
    if (n.type == OrderType::Market) ack.flags |= kEventFlagMarketOrder;
    emit(ack);
    accept(n.id, n.side, limit, n.qty, n.type, n.tif, n.participant, ts);
  }

  void on_cancel(const CancelOrder& c, TsNanos ts) noexcept {
    ++stats_.commands;
    ++stats_.cancels;
    ConstOrderHandle o = book_.find(c.id);
    if (o == nullptr) {
      return reject(c.id, ts,
                    completed_.contains(c.id) ? RejectReason::TooLateToCancel
                                              : RejectReason::UnknownOrder);
    }
    const Qty open = o->qty_open;
    const Side side = o->side;
    const Price price = o->price;
    const ParticipantId participant = o->participant;
    (void)book_.cancel(c.id);
    completed_.insert(c.id);
    emit(make_event(EventType::Cancelled, seq_++, ts, c.id, price, open, 0,
                    side, participant));
  }

  void on_replace(const ReplaceOrder& r, TsNanos ts) noexcept {
    ++stats_.commands;
    ++stats_.replaces;

    ConstOrderHandle o = book_.find(r.id);
    if (o == nullptr) {
      return reject(r.id, ts,
                    completed_.contains(r.id) ? RejectReason::TooLateToCancel
                                              : RejectReason::UnknownOrder);
    }
    if (r.new_qty == 0) return reject(r.id, ts, RejectReason::ZeroQuantity);
    const RejectReason bad = check_price(r.new_price);
    if (bad != RejectReason::None) return reject(r.id, ts, bad);

    const Side side = o->side;
    const Price old_price = o->price;
    const Qty open = o->qty_open;
    const ParticipantId participant = o->participant;

    // Quantity decrease at the same price is the only change that keeps time
    // priority: the client is asking for strictly less, so no one queued
    // behind them is disadvantaged by letting them stay where they are.
    if (r.new_price == old_price && r.new_qty <= open) {
      if (r.new_qty < open) (void)book_.reduce(r.id, open - r.new_qty);
      emit(make_event(EventType::Replaced, seq_++, ts, r.id, r.new_price,
                      r.new_qty, r.new_qty, side, participant));
      return;
    }

    // Everything else loses priority and goes to the tail of the new level.
    //
    // The replace is atomic: the capacity check happens *before* the original
    // order is removed, so a failure leaves the book exactly as it was. A
    // cancel-then-add implementation cannot promise that -- if the add fails
    // the client's order has silently vanished.
    if (!book_.can_add()) return reject(r.id, ts, RejectReason::PoolExhausted);

    (void)book_.cancel(r.id);
    emit(make_event(EventType::Replaced, seq_++, ts, r.id, r.new_price,
                    r.new_qty, r.new_qty, side, participant));

    // A replace that crosses the book is matched by the same code path a new
    // aggressive order uses -- there is only one matching loop in the engine,
    // and it assigns the sequence numbers. See DD-011.
    accept(r.id, side, r.new_price, r.new_qty, OrderType::Limit,
           TimeInForce::Day, participant, ts);
  }

  /// Pops up to \p max commands from the ingress ring and applies them.
  /// Returns how many were processed. This is the consumer end of the SPSC
  /// ring and the only place the engine touches anything shared.
  std::size_t drain(SpscRing<Command, kRingCapacity>& in,
                    std::size_t max) noexcept {
    std::size_t n = 0;
    Command c{};
    while (n < max && in.try_pop(c)) {
      apply(c);
      ++n;
    }
    return n;
  }

  void apply(const Command& c) noexcept {
    switch (c.type) {
    case CommandType::New: on_new(c.body.neworder, c.ts); break;
    case CommandType::Cancel: on_cancel(c.body.cancel, c.ts); break;
    case CommandType::Replace: on_replace(c.body.replace, c.ts); break;
    }
  }

  // -------------------------------------------------------------------------
  // Observation
  // -------------------------------------------------------------------------

  [[nodiscard]] const Book& book() const noexcept { return book_; }
  [[nodiscard]] Book& book() noexcept { return book_; }
  [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
  [[nodiscard]] SeqNum next_seq() const noexcept { return seq_; }
  [[nodiscard]] MatchId last_match() const noexcept { return match_; }
  [[nodiscard]] const EngineConfig& config() const noexcept { return cfg_; }
  [[nodiscard]] const CompletedIdRing& completed() const noexcept {
    return completed_;
  }

  /// Non-mutating: the quantity an order on \p side with limit \p limit could
  /// execute right now, capped at \p want. Public because fill-or-kill
  /// correctness deserves its own tests independent of the matching loop.
  [[nodiscard]] AggQty fillable_qty(Side side, Price limit, AggQty want,
                                    ParticipantId participant) const noexcept {
    AggQty total = 0;
    std::size_t levels = 0;
    const Side other = opposite(side);
    for (Price p = book_.best(other); p != kInvalidPrice;
         p = book_.next_price(other, p)) {
      if (!crosses(side, limit, p)) break;
      if (++levels > cfg_.max_fok_scan_levels) break;
      total += stp_active(participant)
                   ? book_.qty_at_excluding(other, p, participant)
                   : book_.qty_at(other, p);
      if (total >= want) return want; // early exit: bounded by construction
    }
    return total;
  }

private:
  // -------------------------------------------------------------------------
  // The single matching path, shared by on_new and on_replace.
  // -------------------------------------------------------------------------
  void accept(OrderId id, Side side, Price limit, Qty qty, OrderType type,
              TimeInForce tif, ParticipantId participant, TsNanos ts) noexcept {
    Qty remaining = match(id, side, limit, qty, participant, ts);

    if (remaining == 0) {
      completed_.insert(id);
      return;
    }

    const bool may_rest =
        type == OrderType::Limit &&
        (tif == TimeInForce::Day || tif == TimeInForce::PostOnly);

    if (may_rest && book_.add(id, side, limit, remaining, ts, participant))
      return;

    // Either the order is not allowed to rest (IOC, or a market order that
    // ran out of book), or it is allowed but there was no storage left. Both
    // end the order's life; the reason distinguishes them so the client can
    // tell "you asked me not to rest it" from "I ran out of room".
    const RejectReason why =
        may_rest ? RejectReason::PoolExhausted : RejectReason::None;
    completed_.insert(id);
    Event e = make_event(EventType::Cancelled, seq_++, ts, id, limit, remaining,
                         0, side, participant);
    e.reason = why;
    emit(e);
  }

  /// Walks the opposing side from the touch outwards. Returns the unfilled
  /// remainder. This is the only place a trade is created.
  Qty match(OrderId id, Side side, Price limit, Qty qty,
            ParticipantId participant, TsNanos ts) noexcept {
    Qty remaining = qty;
    const Side other = opposite(side);

    while (remaining > 0) {
      OrderHandle h = book_.front(other);
      if (h == nullptr) break;
      if (!crosses(side, limit, h->price)) break;

      if (stp_active(participant) && h->participant == participant) {
        const OrderId resting_id = h->id;
        const Qty resting_open = h->qty_open;
        const Price resting_price = h->price;
        book_.remove(h);
        completed_.insert(resting_id);
        ++stats_.stp_cancels;
        Event e =
            make_event(EventType::Cancelled, seq_++, ts, resting_id,
                       resting_price, resting_open, 0, other, participant);
        e.reason = RejectReason::SelfTradePrevented;
        emit(e);
        continue;
      }

      // The trade prints at the RESTING order's price, never the incoming
      // order's. A buy at 101 hitting a resting ask at 100 trades at 100:
      // the resting order set the terms by being there first, and the
      // aggressor's willingness to pay more is not the market's business.
      const Price trade_price = h->price;
      const Qty fill = std::min(remaining, h->qty_open);
      const OrderId resting_id = h->id;
      const ParticipantId resting_participant = h->participant;
      const Qty resting_after = h->qty_open - fill;

      ++match_;
      ++stats_.trades;
      stats_.shares_traded += fill;
      remaining -= fill;

      Event trade = make_event(EventType::Trade, seq_++, ts, id, trade_price,
                               fill, remaining, side, participant);
      trade.counter_id = resting_id;
      trade.match_id = match_;
      emit(trade);

      // The public print above says what happened; this says what it did to
      // the resting order, whose remaining quantity is otherwise not
      // recoverable from the stream.
      Event fill_ev =
          make_event(EventType::Fill, seq_++, ts, resting_id, trade_price, fill,
                     resting_after, other, resting_participant);
      fill_ev.counter_id = id;
      fill_ev.match_id = match_;
      emit(fill_ev);

      book_.fill(h, fill);
      if (resting_after == 0) completed_.insert(resting_id);
    }
    return remaining;
  }

  [[nodiscard]] bool
  can_fill_entirely(Side side, Price limit, Qty want,
                    ParticipantId participant) const noexcept {
    return fillable_qty(side, limit, want, participant) >= want;
  }

  [[nodiscard]] bool stp_active(ParticipantId participant) const noexcept {
    // Participant 0 means "unattributed". Without this, every default-
    // constructed order would self-trade-prevent against every other one.
    return cfg_.stp != SelfTradePolicy::None && participant != 0;
  }

  [[nodiscard]] RejectReason check_price(Price p) const noexcept {
    if (p <= 0 || p == kInvalidPrice || p == kMarketBuyPrice ||
        p == kMarketSellPrice)
      return RejectReason::InvalidPrice;
    if (p < cfg_.min_price) return RejectReason::PriceOutOfBand;
    if (cfg_.max_price != 0 && p > cfg_.max_price)
      return RejectReason::PriceOutOfBand;
    return RejectReason::None;
  }

  void reject(OrderId id, TsNanos ts, RejectReason why) noexcept {
    ++stats_.rejects;
    Event e =
        make_event(EventType::Reject, seq_++, ts, id, 0, 0, 0, Side::Buy, 0);
    e.reason = why;
    emit(e);
  }

  [[nodiscard]] Event make_event(EventType type, SeqNum seq, TsNanos ts,
                                 OrderId id, Price price, Qty qty,
                                 Qty remaining, Side side,
                                 ParticipantId participant) const noexcept {
    Event e{};
    e.seq = seq;
    e.ts = ts;
    e.id = id;
    e.counter_id = 0;
    e.match_id = 0;
    e.price = price;
    e.qty = qty;
    e.qty_remaining = remaining;
    e.symbol = cfg_.symbol;
    e.participant = participant;
    e.type = type;
    e.side = side;
    e.reason = RejectReason::None;
    e.flags = 0;
    return e;
  }

  void emit(const Event& e) noexcept { sink_.emit(e); }

  Book book_;
  Sink& sink_;
  EngineConfig cfg_;
  CompletedIdRing completed_;
  SeqNum seq_{0};
  MatchId match_{0};
  EngineStats stats_{};
};

} // namespace miniex
