/// \file book_interface.hpp
/// \brief The contract every order book implementation satisfies.
///
/// **Mechanism, not policy.** The book owns storage, price ordering, FIFO
/// ordering within a level, and level aggregates. It does *not* match. All
/// matching policy -- which order types cross, what price a trade prints at,
/// what a replace does to priority -- lives in `engine/matching_engine.hpp`.
///
/// The separation is what makes the differential testing in M3/M5 possible:
/// `NaiveBook` and `FastBook` are two implementations of the same mechanism,
/// so running the same engine over both must produce byte-identical event
/// streams. If matching lived in the book there would be two implementations
/// of the policy too, and agreement would prove much less.
///
/// A C++20 concept rather than an abstract base class: the engine is
/// templated on the book, so every call inlines and there is no vtable on
/// the hot path.
#pragma once

#include <concepts>
#include <cstddef>

#include "core/types.hpp"

namespace miniex {

/// Books hand out raw `Order*` handles. They are stable for the lifetime of
/// the resting order in both implementations (a `std::list` node in
/// `NaiveBook`, a slab slot in `FastBook`), and they are never owned by the
/// caller -- the book frees the storage in `remove()`.
using OrderHandle = Order*;
using ConstOrderHandle = const Order*;

// clang-format off
template <class B>
concept OrderBook = requires(B b, const B cb, OrderId id, Price p, Side s,
                             Qty q, TsNanos ts, ParticipantId pid,
                             OrderHandle h) {
  // --- mutation -----------------------------------------------------------
  /// Insert a new resting order at the tail of its price level.
  /// False means storage is exhausted; the book is unchanged.
  { b.add(id, s, p, q, ts, pid) } -> std::same_as<bool>;

  /// Remove an order by id. False if the id is not resting.
  { b.cancel(id) }               -> std::same_as<bool>;

  /// Reduce an order's open quantity, keeping its position in the FIFO.
  /// Removes the order if the reduction takes it to zero. False if unknown
  /// or if `q` exceeds the open quantity.
  { b.reduce(id, q) }            -> std::same_as<bool>;

  /// Reduce by handle, as the matching loop does. `q` must not exceed the
  /// order's open quantity; the order is removed when it reaches zero.
  { b.fill(h, q) }               -> std::same_as<void>;

  /// Remove by handle, as self-trade prevention does.
  { b.remove(h) }                -> std::same_as<void>;

  { b.clear() }                  -> std::same_as<void>;

  // --- traversal ----------------------------------------------------------
  /// Best (most aggressive) price on `s`, or kInvalidPrice if empty.
  { cb.best(s) }                 -> std::same_as<Price>;

  /// The next price level away from the touch after `p`, or kInvalidPrice.
  /// Walking best() then next_price() repeatedly visits every level in
  /// price priority order, which is how the FOK pre-scan works without
  /// mutating anything.
  { cb.next_price(s, p) }        -> std::same_as<Price>;

  /// Aggregate open quantity resting at one price. 0 if the level is empty.
  { cb.qty_at(s, p) }            -> std::same_as<AggQty>;

  /// Number of orders resting at one price.
  { cb.orders_at(s, p) }         -> std::same_as<std::size_t>;

  /// Aggregate open quantity at one price, ignoring orders belonging to
  /// `pid`. Participant 0 means "unattributed" and excludes nothing, so
  /// `qty_at_excluding(s, p, 0) == qty_at(s, p)`. This exists so the FOK
  /// pre-scan can stay non-mutating when self-trade prevention is on: STP
  /// cancels same-participant resting orders rather than trading with them,
  /// so they are not liquidity as far as a fill-or-kill check is concerned.
  { cb.qty_at_excluding(s, p, pid) } -> std::same_as<AggQty>;

  /// The oldest order at the best price on `s` -- the next one to be filled.
  /// nullptr if the side is empty.
  { b.front(s) }                 -> std::same_as<OrderHandle>;

  /// Look up a resting order by id; nullptr if not resting.
  { cb.find(id) }                -> std::same_as<ConstOrderHandle>;

  // --- accounting ---------------------------------------------------------
  { cb.order_count() }           -> std::same_as<std::size_t>;

  /// False when the next add() is certain to fail. Checked *before* a
  /// replace removes the original order, so a failed replace can leave the
  /// original untouched (spec 8.5).
  { cb.can_add() }               -> std::same_as<bool>;
};
// clang-format on

} // namespace miniex
