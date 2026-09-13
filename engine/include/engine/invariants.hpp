/// \file invariants.hpp
/// \brief The eight invariants of spec section 8.8, as executable checks.
///
/// Split in two, because they are checked against different things:
///
///   * `check_book()` looks at the book's internal state: no crossed book,
///     level aggregates equal to the sum of their members, index count equal
///     to the sum of level counts, FIFO order by acceptance time.
///   * `ShareLedger` looks at the *event stream*: share conservation per
///     order, trade prices inside both parties' limits, and buy-side traded
///     quantity equal to sell-side traded quantity.
///
/// The book checks are O(orders) and are for debug builds and tests only --
/// never call them from the hot path. The ledger is O(1) per event and is
/// cheap enough to run over a whole replay.
///
/// A note on scope: the "book is never crossed" invariant applies to the
/// matching engine only. `MarketDataBook` (spec section 11) reconstructs what
/// a real exchange published, and real feeds do produce transiently locked
/// and crossed books. Asserting no-cross there is the classic reconstruction
/// bug; see docs/validation.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.hpp"

#include "engine/messages.hpp"

namespace miniex {

struct InvariantViolation {
  std::string what;
  [[nodiscard]] bool ok() const { return what.empty(); }
  explicit operator bool() const { return what.empty(); }
};

/// Checks invariants 1, 3, 4 and 5 against a book's own state.
///
/// Templated rather than taking the `OrderBook` concept because it needs the
/// level/order iteration helpers, which are inspection APIs rather than part
/// of the matching contract.
template <class Book>
InvariantViolation check_book(const Book& book) {
  // (1) The book is never crossed or locked after an operation completes.
  const Price bid = book.best(Side::Buy);
  const Price ask = book.best(Side::Sell);
  if (bid != kInvalidPrice && ask != kInvalidPrice && bid >= ask) {
    return {"crossed or locked book: best bid " + std::to_string(bid) +
            " >= best ask " + std::to_string(ask)};
  }

  std::size_t counted_orders = 0;
  for (const Side side : {Side::Buy, Side::Sell}) {
    Price previous = kInvalidPrice;
    bool have_previous = false;
    std::string failure;

    book.for_each_level(side, [&](Price price, AggQty total, std::size_t n) {
      if (!failure.empty()) return;

      // Levels must be visited from the touch outwards, strictly monotone.
      if (have_previous && !better_price(side, previous, price)) {
        failure = "levels not in price priority order on side " +
                  std::string(1, side_char(side)) + ": " +
                  std::to_string(previous) + " then " + std::to_string(price);
        return;
      }
      previous = price;
      have_previous = true;

      if (n == 0) {
        failure = "empty level left in the book at " + std::to_string(price);
        return;
      }

      // (3) Level aggregate equals the sum of its FIFO members' open
      //     quantities, and (5) entry timestamps are non-decreasing from
      //     head to tail.
      AggQty walked = 0;
      std::size_t walked_count = 0;
      TsNanos last_ts = 0;
      bool first = true;
      book.for_each_order(side, price, [&](const Order& o) {
        walked += o.qty_open;
        ++walked_count;
        if (o.qty_open == 0 && failure.empty())
          failure = "zero-quantity order " + std::to_string(o.id) +
                    " left resting at " + std::to_string(price);
        if (o.price != price && failure.empty())
          failure = "order " + std::to_string(o.id) + " at level " +
                    std::to_string(price) + " carries price " +
                    std::to_string(o.price);
        if (!first && o.ts_entry < last_ts && failure.empty())
          failure = "FIFO violated at " + std::to_string(price) +
                    ": entry time went backwards at order " +
                    std::to_string(o.id);
        last_ts = o.ts_entry;
        first = false;
      });

      if (!failure.empty()) return;
      if (walked != total) {
        failure = "level aggregate wrong at " + std::to_string(price) +
                  ": stored " + std::to_string(total) + ", members sum to " +
                  std::to_string(walked);
        return;
      }
      if (walked_count != n) {
        failure = "level order count wrong at " + std::to_string(price) +
                  ": stored " + std::to_string(n) + ", walked " +
                  std::to_string(walked_count);
        return;
      }
      counted_orders += walked_count;
    });

    if (!failure.empty()) return {failure};
  }

  // (4) The id index agrees with the sum of the level counts.
  if (counted_orders != book.order_count()) {
    return {"index size " + std::to_string(book.order_count()) +
            " disagrees with walked order count " +
            std::to_string(counted_orders)};
  }
  return {};
}

/// Checks invariants 2, 6 and 7 by consuming the outbound event stream.
///
/// It knows nothing about the book, which is the point: if the book and the
/// ledger both say shares are conserved, two independent things agree.
class ShareLedger {
public:
  struct OrderState {
    Qty original{0};
    Qty filled{0};
    Qty open{0};
    Qty cancelled{0};
    Price limit{0};
    Side side{Side::Buy};
    bool is_market{false};
  };

  void on_event(const Event& e) {
    switch (e.type) {
    case EventType::Ack: {
      OrderState& s = orders_[e.id];
      // A replace re-acks nothing, so an Ack is always a fresh order.
      s = OrderState{e.qty,
                     0,
                     e.qty,
                     0,
                     e.price,
                     e.side,
                     (e.flags & kEventFlagMarketOrder) != 0};
      break;
    }
    case EventType::Replaced: {
      // A replace resets the order's accounting to the new quantity: the
      // shares already filled stay filled, the new quantity is the new
      // working amount.
      const auto it = orders_.find(e.id);
      if (it == orders_.end()) {
        fail("REPLACED for an order that was never acked: " +
             std::to_string(e.id));
        return;
      }
      it->second.original = static_cast<Qty>(it->second.filled + e.qty);
      it->second.open = e.qty;
      it->second.limit = e.price;
      break;
    }
    case EventType::Trade: {
      // (7) Every trade contributes equally to both sides' totals; a
      //     mismatch means a trade was booked on one side only.
      if (e.side == Side::Buy) {
        buy_qty_ += e.qty;
      } else {
        sell_qty_ += e.qty;
      }
      apply_execution(e.id, e);
      // (6) The print must be inside both parties' limit prices.
      check_trade_price(e);
      break;
    }
    case EventType::Fill:
      apply_execution(e.id, e);
      // The resting side of the same match; counted on its own side so
      // that buy and sell totals balance.
      if (e.side == Side::Buy) {
        buy_qty_ += e.qty;
      } else {
        sell_qty_ += e.qty;
      }
      break;
    case EventType::Cancelled: {
      const auto it = orders_.find(e.id);
      if (it == orders_.end()) {
        fail("CANCELLED for an order that was never acked: " +
             std::to_string(e.id));
        return;
      }
      if (e.qty > it->second.open) {
        fail("cancelled " + std::to_string(e.qty) + " of order " +
             std::to_string(e.id) + " which had only " +
             std::to_string(it->second.open) + " open");
        return;
      }
      it->second.open = static_cast<Qty>(it->second.open - e.qty);
      it->second.cancelled = static_cast<Qty>(it->second.cancelled + e.qty);
      break;
    }
    case EventType::Reject:
    case EventType::BookDelta: break;
    }
    if (e.seq != expected_seq_) {
      fail("sequence number gap: expected " + std::to_string(expected_seq_) +
           ", saw " + std::to_string(e.seq));
    }
    expected_seq_ = e.seq + 1;
  }

  /// (2) For every order ever accepted: orig == filled + open + cancelled.
  [[nodiscard]] InvariantViolation check() const {
    if (!failure_.empty()) return {failure_};
    for (const auto& [id, s] : orders_) {
      const std::uint64_t sum =
          static_cast<std::uint64_t>(s.filled) + s.open + s.cancelled;
      if (sum != s.original) {
        return {"share conservation broken for order " + std::to_string(id) +
                ": orig " + std::to_string(s.original) + " != filled " +
                std::to_string(s.filled) + " + open " + std::to_string(s.open) +
                " + cancelled " + std::to_string(s.cancelled)};
      }
    }
    if (buy_qty_ != sell_qty_) {
      return {"traded quantity does not balance: buy " +
              std::to_string(buy_qty_) + " vs sell " +
              std::to_string(sell_qty_)};
    }
    return {};
  }

  [[nodiscard]] std::uint64_t traded_shares() const { return buy_qty_; }
  [[nodiscard]] const OrderState* state(OrderId id) const {
    const auto it = orders_.find(id);
    return it == orders_.end() ? nullptr : &it->second;
  }

private:
  void apply_execution(OrderId id, const Event& e) {
    const auto it = orders_.find(id);
    if (it == orders_.end()) {
      fail("execution for an order that was never acked: " +
           std::to_string(id));
      return;
    }
    if (e.qty > it->second.open) {
      fail("filled " + std::to_string(e.qty) + " of order " +
           std::to_string(id) + " which had only " +
           std::to_string(it->second.open) + " open");
      return;
    }
    it->second.filled = static_cast<Qty>(it->second.filled + e.qty);
    it->second.open = static_cast<Qty>(it->second.open - e.qty);
    if (it->second.open != e.qty_remaining) {
      fail("order " + std::to_string(id) + " reports remaining " +
           std::to_string(e.qty_remaining) + " but the ledger has " +
           std::to_string(it->second.open));
    }
  }

  void check_trade_price(const Event& e) {
    const OrderState* aggressor = state(e.id);
    const OrderState* resting = state(e.counter_id);
    if (aggressor == nullptr || resting == nullptr) return;
    // A market order has no limit, so only the resting side constrains.
    if (!aggressor->is_market) {
      if (!crosses(aggressor->side, aggressor->limit, e.price)) {
        fail("trade at " + std::to_string(e.price) +
             " violates the aggressor's limit " +
             std::to_string(aggressor->limit));
      }
    }
    if (e.price != resting->limit) {
      fail("trade printed at " + std::to_string(e.price) +
           " but the resting order's price was " +
           std::to_string(resting->limit) +
           " -- trades print at the resting "
           "price");
    }
  }

  void fail(std::string message) {
    if (failure_.empty()) failure_ = std::move(message);
  }

  std::unordered_map<OrderId, OrderState> orders_;
  std::uint64_t buy_qty_{0};
  std::uint64_t sell_qty_{0};
  SeqNum expected_seq_{0};
  std::string failure_;
};

} // namespace miniex
