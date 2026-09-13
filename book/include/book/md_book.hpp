/// \file md_book.hpp
/// \brief The market-data reconstruction book: what the exchange published.
///
/// This is a **different kind of book** from `FastBook`, and conflating the
/// two is the standard way to get reconstruction wrong.
///
/// `FastBook` is a matching book: it holds orders the engine owns, it enforces
/// price-time priority, and the engine guarantees it is never crossed.
/// `MarketDataBook` holds the exchange's orders, reconstructed from messages
/// that merely describe what already happened. It has no matching, no order
/// types, no time-in-force, and — critically —
///
/// > **the no-crossed-book invariant does not apply here.**
///
/// Real feeds produce transiently locked and crossed books. A `U` replace can
/// move an order to a price through the opposite side, and for the handful of
/// messages until the other side reacts, best bid ≥ best ask. A reconstructor
/// that asserts no-cross dies on real data on a normal day. See
/// `docs/validation.md`.
///
/// ## What is reconstructed, and what is not
///
/// Per-order records keyed by reference number (so it is a genuine L3
/// reconstruction) plus incrementally maintained per-level aggregates (so the
/// L2 view LOBSTER publishes is a lookup, not a walk).
///
/// **Queue position within a level is deliberately not maintained.** Nothing
/// observable depends on it here: this book never matches, and neither
/// LOBSTER's book file nor any depth feed exposes the order of a level's
/// queue. Maintaining an intrusive list would cost memory and code for a
/// property no test could check. `FastBook` maintains it because there it
/// decides who gets filled.
///
/// ## Errors are counted, not asserted
///
/// An execution, cancel or delete for a reference number the book has never
/// seen is normal when a file is joined mid-day, and it happens at the start
/// of every sample that does not begin at the opening cross. These are
/// counted and reported. `--strict` turns them into errors, for a run that is
/// known to start from the beginning of the session.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "core/open_hash_map.hpp"
#include "core/types.hpp"

namespace miniex {

/// One aggregated price level in the reconstructed book.
struct MdLevel {
  AggQty total_qty{0};
  std::uint32_t order_count{0};
};

/// A single level of the top-of-book view, as a depth feed would publish it.
struct DepthLevel {
  Price price{kInvalidPrice};
  AggQty qty{0};
  std::uint32_t orders{0};

  friend bool operator==(const DepthLevel& a, const DepthLevel& b) {
    return a.price == b.price && a.qty == b.qty;
  }
};

/// Counters for the things that are expected but noteworthy.
struct MdStats {
  std::uint64_t adds{0};
  std::uint64_t executes{0};
  std::uint64_t cancels{0};
  std::uint64_t deletes{0};
  std::uint64_t replaces{0};
  std::uint64_t hidden_prints{0};
  std::uint64_t crosses{0};
  std::uint64_t broken{0};

  /// An E/C/X/D/U for a reference number never seen. Normal when starting
  /// mid-day; an error under --strict.
  std::uint64_t unknown_reference{0};
  /// A reduction larger than the order's remaining shares. Should never
  /// happen on a clean feed read from the start; if it does, either the
  /// reconstruction or the reading of the protocol is wrong.
  std::uint64_t oversized_reduction{0};
  /// A duplicate reference number for a live order.
  std::uint64_t duplicate_reference{0};

  /// Printable executions only. Non-printable ones change the book but are
  /// excluded from volume by definition.
  std::uint64_t printable_volume{0};
  std::uint64_t printable_trades{0};
  Price last_trade_price{kInvalidPrice};

  /// Messages that left the book crossed or locked. Not an error -- see the
  /// file comment -- but worth reporting, because a count of zero over a full
  /// day would itself be suspicious.
  std::uint64_t crossed_states{0};
};

class MarketDataBook {
public:
  /// \param max_orders the ceiling on simultaneously live reference numbers.
  /// The index is sized for it up front; the order records start small and
  /// grow, because a full day for one symbol rarely has more than a few tens
  /// of thousands of orders live at once and pre-allocating the ceiling would
  /// be tens of megabytes of untouched pages.
  explicit MarketDataBook(std::size_t max_orders = 1U << 20)
      : index_(max_orders), orders_(1U << 12) {
    free_list_.reserve(1024);
  }

  // -------------------------------------------------------------------------
  // The message handlers of spec section 11
  // -------------------------------------------------------------------------

  /// `A` / `F`: insert a new order.
  bool add(OrderId ref, Side side, Price price, Qty shares) {
    if (index_.find(ref) != kInvalidIndex) {
      ++stats_.duplicate_reference;
      return false;
    }
    const std::uint32_t slot = allocate();
    orders_[slot] = Record{ref, price, shares, side, true};
    if (!index_.insert(ref, slot)) {
      release(slot);
      ++stats_.duplicate_reference;
      return false;
    }
    MdLevel& level = level_for(side, price);
    level.total_qty += shares;
    ++level.order_count;
    ++stats_.adds;
    note_cross();
    return true;
  }

  /// `E`: reduce by executed shares; the trade prints at the order's own
  /// resting price, which the message does not carry.
  bool execute(OrderId ref, Qty shares, bool printable = true) {
    const std::uint32_t slot = index_.find(ref);
    if (slot == kInvalidIndex) {
      ++stats_.unknown_reference;
      return false;
    }
    const Price price = orders_[slot].price;
    if (!reduce_slot(slot, shares)) return false;
    ++stats_.executes;
    record_trade(price, shares, printable);
    note_cross();
    return true;
  }

  /// `C`: the same reduction, but the trade prints at the message's own
  /// execution price, and may be marked non-printable.
  bool execute_with_price(OrderId ref, Qty shares, Price price,
                          bool printable) {
    const std::uint32_t slot = index_.find(ref);
    if (slot == kInvalidIndex) {
      ++stats_.unknown_reference;
      return false;
    }
    if (!reduce_slot(slot, shares)) return false;
    ++stats_.executes;
    record_trade(price, shares, printable);
    note_cross();
    return true;
  }

  /// `X`: reduce by cancelled shares. The order usually survives.
  bool cancel(OrderId ref, Qty shares) {
    const std::uint32_t slot = index_.find(ref);
    if (slot == kInvalidIndex) {
      ++stats_.unknown_reference;
      return false;
    }
    if (!reduce_slot(slot, shares)) return false;
    ++stats_.cancels;
    note_cross();
    return true;
  }

  /// `D`: remove the order entirely.
  bool remove(OrderId ref) {
    const std::uint32_t slot = index_.find(ref);
    if (slot == kInvalidIndex) {
      ++stats_.unknown_reference;
      return false;
    }
    detach(slot);
    ++stats_.deletes;
    note_cross();
    return true;
  }

  /// `U`: remove the original reference and insert the new one. The new order
  /// is a different order at a new price with a new reference number; the old
  /// reference is dead.
  bool replace(OrderId old_ref, OrderId new_ref, Qty shares, Price price) {
    const std::uint32_t slot = index_.find(old_ref);
    if (slot == kInvalidIndex) {
      ++stats_.unknown_reference;
      return false;
    }
    const Side side = orders_[slot].side;
    detach(slot);
    const bool ok = add(new_ref, side, price, shares);
    if (ok) {
      --stats_.adds; // the add is part of the replace, not a separate add
      ++stats_.replaces;
    }
    return ok;
  }

  /// `P`: a non-displayed execution. A trade print with **no book change** --
  /// it matched against liquidity this book never saw.
  void hidden_print(Price price, Qty shares) {
    ++stats_.hidden_prints;
    record_trade(price, shares, true);
  }

  /// `Q`: an auction cross. A trade print with no book change.
  void cross_print(Price price, Qty shares) {
    ++stats_.crosses;
    record_trade(price, shares, true);
  }

  /// `B`: a broken trade. Adjusts statistics only; the book is untouched.
  /// The volume is not un-counted here because doing so correctly needs the
  /// match number's original quantity, which means remembering every trade.
  /// The count is reported instead, so a reader knows how many prints in the
  /// statistics were later broken.
  void broken_trade() { ++stats_.broken; }

  // -------------------------------------------------------------------------
  // Views
  // -------------------------------------------------------------------------

  [[nodiscard]] Price best(Side side) const {
    // Both sides use one ascending map, so "best" is the last key for a bid
    // and the first for an ask. Two maps with different comparators would
    // read more directly but cannot be selected between at runtime -- they
    // are different types -- and every accessor would need duplicating.
    if (side == Side::Buy)
      return bids_.empty() ? kInvalidPrice : bids_.rbegin()->first;
    return asks_.empty() ? kInvalidPrice : asks_.begin()->first;
  }

  [[nodiscard]] AggQty qty_at(Side side, Price price) const {
    const LevelMap& levels = levels_for(side);
    const auto it = levels.find(price);
    return it == levels.end() ? 0 : it->second.total_qty;
  }

  /// The side and price a reference number is resting at, or a default if it
  /// is unknown. Kept narrow on purpose: `tape_print` needs the resting
  /// order's side to derive the aggressor's, and an execution message does
  /// not repeat it.
  [[nodiscard]] Side side_of(OrderId ref) const {
    const std::uint32_t slot = index_.find(ref);
    return slot == kInvalidIndex ? Side::Buy : orders_[slot].side;
  }
  [[nodiscard]] Price price_of(OrderId ref) const {
    const std::uint32_t slot = index_.find(ref);
    return slot == kInvalidIndex ? kInvalidPrice : orders_[slot].price;
  }
  [[nodiscard]] Qty qty_of(OrderId ref) const {
    const std::uint32_t slot = index_.find(ref);
    return slot == kInvalidIndex ? 0 : orders_[slot].shares;
  }

  [[nodiscard]] std::size_t order_count() const { return live_; }
  [[nodiscard]] std::size_t level_count(Side side) const {
    return levels_for(side).size();
  }
  [[nodiscard]] const MdStats& stats() const { return stats_; }

  /// True when best bid >= best ask. Real feeds do this; it is reported, not
  /// asserted.
  [[nodiscard]] bool is_crossed_or_locked() const {
    const Price bid = best(Side::Buy);
    const Price ask = best(Side::Sell);
    return bid != kInvalidPrice && ask != kInvalidPrice && bid >= ask;
  }

  /// The top \p n levels of one side, from the touch outwards. Missing levels
  /// are reported with `price == kInvalidPrice` and zero quantity, which is
  /// how LOBSTER represents a book thinner than its configured depth.
  void top_n(Side side, std::size_t n, std::vector<DepthLevel>& out) const {
    out.clear();
    out.reserve(n);
    for_each_level(side, [&](Price price, AggQty qty, std::size_t orders) {
      if (out.size() < n)
        out.push_back(
            DepthLevel{price, qty, static_cast<std::uint32_t>(orders)});
    });
    while (out.size() < n) out.push_back(DepthLevel{});
  }

  [[nodiscard]] std::vector<DepthLevel> top_n(Side side, std::size_t n) const {
    std::vector<DepthLevel> out;
    top_n(side, n, out);
    return out;
  }

  /// Visits levels from the touch outwards: descending for bids, ascending
  /// for asks.
  template <class Fn>
  void for_each_level(Side side, Fn&& fn) const {
    if (side == Side::Buy) {
      for (auto it = bids_.rbegin(); it != bids_.rend(); ++it)
        fn(it->first, it->second.total_qty,
           static_cast<std::size_t>(it->second.order_count));
    } else {
      for (const auto& [price, level] : asks_)
        fn(price, level.total_qty, static_cast<std::size_t>(level.order_count));
    }
  }

  void clear() {
    bids_.clear();
    asks_.clear();
    index_.clear();
    free_list_.clear();
    next_slot_ = 0;
    live_ = 0;
    stats_ = MdStats{};
  }

  /// Recomputes every level aggregate from the order records and compares.
  /// Test and debug only.
  [[nodiscard]] bool verify_aggregates() const {
    std::map<Price, MdLevel> recomputed[2];
    std::size_t counted = 0;
    index_.for_each([&](OrderId, std::uint32_t slot) {
      const Record& r = orders_[slot];
      MdLevel& level = recomputed[index_of(r.side)][r.price];
      level.total_qty += r.shares;
      ++level.order_count;
      ++counted;
    });
    if (counted != live_) return false;
    for (const Side side : {Side::Buy, Side::Sell}) {
      const LevelMap& levels = levels_for(side);
      const auto& expected = recomputed[index_of(side)];
      if (levels.size() != expected.size()) return false;
      for (const auto& [price, level] : levels) {
        const auto it = expected.find(price);
        if (it == expected.end()) return false;
        if (it->second.total_qty != level.total_qty) return false;
        if (it->second.order_count != level.order_count) return false;
      }
    }
    return true;
  }

private:
  struct Record {
    OrderId ref{0};
    Price price{0};
    Qty shares{0};
    Side side{Side::Buy};
    bool live{false};
  };

  std::uint32_t allocate() {
    if (!free_list_.empty()) {
      const std::uint32_t slot = free_list_.back();
      free_list_.pop_back();
      ++live_;
      return slot;
    }
    if (next_slot_ >= orders_.size()) orders_.resize(orders_.size() * 2);
    ++live_;
    return static_cast<std::uint32_t>(next_slot_++);
  }

  void release(std::uint32_t slot) {
    orders_[slot].live = false;
    free_list_.push_back(slot);
    --live_;
  }

  using LevelMap = std::map<Price, MdLevel>;

  [[nodiscard]] const LevelMap& levels_for(Side side) const {
    return side == Side::Buy ? bids_ : asks_;
  }
  [[nodiscard]] LevelMap& levels_for(Side side) {
    return side == Side::Buy ? bids_ : asks_;
  }

  MdLevel& level_for(Side side, Price price) { return levels_for(side)[price]; }

  /// Reduces an order's shares, removing it if it reaches zero.
  bool reduce_slot(std::uint32_t slot, Qty shares) {
    Record& r = orders_[slot];
    if (shares > r.shares) {
      ++stats_.oversized_reduction;
      // Clamp rather than go negative: a negative aggregate would corrupt
      // every later comparison, and the counter above says it happened.
      shares = r.shares;
    }
    LevelMap& levels = levels_for(r.side);
    const auto it = levels.find(r.price);
    if (it != levels.end()) it->second.total_qty -= shares;
    r.shares -= shares;
    if (r.shares == 0) detach(slot);
    return true;
  }

  void detach(std::uint32_t slot) {
    Record& r = orders_[slot];
    LevelMap& levels = levels_for(r.side);
    const auto it = levels.find(r.price);
    if (it != levels.end()) {
      it->second.total_qty -= r.shares;
      if (it->second.order_count > 0) --it->second.order_count;
      if (it->second.order_count == 0) levels.erase(it);
    }
    (void)index_.erase(r.ref);
    release(slot);
  }

  void record_trade(Price price, Qty shares, bool printable) {
    if (!printable) return;
    stats_.printable_volume += shares;
    ++stats_.printable_trades;
    stats_.last_trade_price = price;
  }

  void note_cross() {
    if (is_crossed_or_locked()) ++stats_.crossed_states;
  }

  OpenHashMap index_;
  std::vector<Record> orders_;
  std::vector<std::uint32_t> free_list_;
  std::size_t next_slot_{0};
  std::size_t live_{0};
  // One ordering for both sides; see best() for why. Bids are read in
  // reverse.
  LevelMap bids_;
  LevelMap asks_;
  MdStats stats_;
};

} // namespace miniex
