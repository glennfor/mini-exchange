/// \file naive_book.hpp
/// \brief The deliberately obvious order book. Never optimised, never removed.
///
/// `std::map` of price to `std::list` of orders, plus a `std::unordered_map`
/// from order id to a locator. It allocates, it chases pointers, and its
/// cancel path is a hash lookup followed by a list splice. All of that is
/// fine, because its job is to be *obviously correct* and to serve as the
/// in-repo oracle that `FastBook` is differentially tested against forever.
///
/// Two rules for this file:
///   1. Never optimise it. The moment it becomes clever it stops being an
///      independent check on the clever one.
///   2. Never delete it. It stays in CI for the life of the project.
#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>

#include "core/types.hpp"

#include "book/book_interface.hpp"

namespace miniex {

class NaiveBook {
public:
  struct Level {
    std::list<Order> orders;
    AggQty total_qty{0};
  };

  NaiveBook() = default;

  // ---------------------------------------------------------------------
  // Mutation
  // ---------------------------------------------------------------------

  bool add(OrderId id, Side side, Price price, Qty qty, TsNanos ts,
           ParticipantId participant) {
    if (index_.find(id) != index_.end()) return false;
    Level& level = side == Side::Buy ? bids_[price] : asks_[price];

    Order o{};
    o.price = price;
    o.ts_entry = ts;
    o.id = id;
    o.qty_open = qty;
    o.qty_orig = qty;
    o.next = kInvalidIndex;
    o.prev = kInvalidIndex;
    o.level = kInvalidLevel;
    o.participant = participant;
    o.side = side;
    o.type = OrderType::Limit;
    o.tif = TimeInForce::Day;
    o.flags = kOrderFlagNone;

    level.orders.push_back(o);
    level.total_qty += qty;
    index_.emplace(id, Locator{side, price, std::prev(level.orders.end())});
    return true;
  }

  bool cancel(OrderId id) {
    const auto it = index_.find(id);
    if (it == index_.end()) return false;
    erase_located(it);
    return true;
  }

  bool reduce(OrderId id, Qty by) {
    const auto it = index_.find(id);
    if (it == index_.end()) return false;
    Order& o = *it->second.it;
    if (by > o.qty_open) return false;
    level_of(it->second)->total_qty -= by;
    o.qty_open -= by;
    if (o.qty_open == 0) erase_located(it);
    return true;
  }

  void fill(OrderHandle h, Qty qty) {
    const auto it = index_.find(h->id);
    Level* level = level_of(it->second);
    level->total_qty -= qty;
    h->qty_open -= qty;
    h->flags |= kOrderFlagTraded;
    if (h->qty_open == 0) erase_located(it);
  }

  void remove(OrderHandle h) { (void)cancel(h->id); }

  void clear() {
    bids_.clear();
    asks_.clear();
    index_.clear();
  }

  // ---------------------------------------------------------------------
  // Traversal
  // ---------------------------------------------------------------------

  [[nodiscard]] Price best(Side side) const {
    if (side == Side::Buy)
      return bids_.empty() ? kInvalidPrice : bids_.begin()->first;
    return asks_.empty() ? kInvalidPrice : asks_.begin()->first;
  }

  /// Both maps are ordered from the touch outwards (bids descending, asks
  /// ascending), so "the next level away from the touch" is `upper_bound`
  /// under each map's own comparator in both cases.
  [[nodiscard]] Price next_price(Side side, Price after) const {
    if (side == Side::Buy) {
      const auto it = bids_.upper_bound(after);
      return it == bids_.end() ? kInvalidPrice : it->first;
    }
    const auto it = asks_.upper_bound(after);
    return it == asks_.end() ? kInvalidPrice : it->first;
  }

  [[nodiscard]] AggQty qty_at(Side side, Price price) const {
    const Level* level = find_level(side, price);
    return level == nullptr ? 0 : level->total_qty;
  }

  [[nodiscard]] AggQty qty_at_excluding(Side side, Price price,
                                        ParticipantId ignore) const {
    const Level* level = find_level(side, price);
    if (level == nullptr) return 0;
    if (ignore == 0) return level->total_qty;
    AggQty total = 0;
    for (const Order& o : level->orders)
      if (o.participant != ignore) total += o.qty_open;
    return total;
  }

  [[nodiscard]] std::size_t orders_at(Side side, Price price) const {
    const Level* level = find_level(side, price);
    return level == nullptr ? 0 : level->orders.size();
  }

  [[nodiscard]] OrderHandle front(Side side) {
    if (side == Side::Buy) {
      if (bids_.empty()) return nullptr;
      return &bids_.begin()->second.orders.front();
    }
    if (asks_.empty()) return nullptr;
    return &asks_.begin()->second.orders.front();
  }

  [[nodiscard]] ConstOrderHandle find(OrderId id) const {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &*it->second.it;
  }

  [[nodiscard]] OrderHandle find_mutable(OrderId id) {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &*it->second.it;
  }

  // ---------------------------------------------------------------------
  // Accounting
  // ---------------------------------------------------------------------

  [[nodiscard]] std::size_t order_count() const { return index_.size(); }
  [[nodiscard]] bool can_add() const { return true; }
  [[nodiscard]] std::size_t level_count(Side side) const {
    return side == Side::Buy ? bids_.size() : asks_.size();
  }

  // ---------------------------------------------------------------------
  // Inspection, for tests, the invariant checker and the book_dump tool
  // ---------------------------------------------------------------------

  /// Visits every level from the touch outwards: fn(price, total_qty, count).
  template <class Fn>
  void for_each_level(Side side, Fn&& fn) const {
    if (side == Side::Buy) {
      for (const auto& [price, level] : bids_)
        fn(price, level.total_qty, level.orders.size());
    } else {
      for (const auto& [price, level] : asks_)
        fn(price, level.total_qty, level.orders.size());
    }
  }

  /// Visits every order at one price, head (oldest) first.
  template <class Fn>
  void for_each_order(Side side, Price price, Fn&& fn) const {
    const Level* level = find_level(side, price);
    if (level == nullptr) return;
    for (const Order& o : level->orders) fn(o);
  }

private:
  using ListIter = std::list<Order>::iterator;
  struct Locator {
    Side side;
    Price price;
    ListIter it;
  };

  Level* level_of(const Locator& loc) {
    if (loc.side == Side::Buy) return &bids_.find(loc.price)->second;
    return &asks_.find(loc.price)->second;
  }

  [[nodiscard]] const Level* find_level(Side side, Price price) const {
    if (side == Side::Buy) {
      const auto it = bids_.find(price);
      return it == bids_.end() ? nullptr : &it->second;
    }
    const auto it = asks_.find(price);
    return it == asks_.end() ? nullptr : &it->second;
  }

  void erase_located(std::unordered_map<OrderId, Locator>::iterator it) {
    const Locator loc = it->second;
    Level* level = level_of(loc);
    level->total_qty -= loc.it->qty_open;
    level->orders.erase(loc.it);
    if (level->orders.empty()) {
      if (loc.side == Side::Buy)
        bids_.erase(loc.price);
      else
        asks_.erase(loc.price);
    }
    index_.erase(it);
  }

  // Bids descend from the touch, asks ascend from it, so begin() is the
  // best price on both sides and iteration order is price priority order.
  std::map<Price, Level, std::greater<>> bids_;
  std::map<Price, Level, std::less<>> asks_;
  std::unordered_map<OrderId, Locator> index_;
};

static_assert(OrderBook<NaiveBook>);

} // namespace miniex
