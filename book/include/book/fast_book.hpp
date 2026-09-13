/// \file fast_book.hpp
/// \brief The production order book: price ladder, intrusive FIFOs, slab
///        pool, hierarchical best-price bitset.
///
/// Everything here exists because `NaiveBook` does the same job in an
/// obviously correct way and `FastBook` has to agree with it on a million
/// random command sequences. That agreement is the licence to be clever.
///
/// ## The four structures
///
/// **Price ladder.** A flat array of `PriceLevel`, one per tick, indexed by
/// `(price - base) / tick`. Level lookup is an integer subtract, a shift and
/// a load: no tree descent, no hashing. The array is a *window* — 131072
/// ticks, or $13.11 at four implied decimals — and prices outside it fall
/// into a per-side overflow `std::map`. Both paths are exercised by the
/// differential tests, and the overflow path has its own benchmark pattern
/// (`--pattern ladder-overflow`), because an untested worst case is not a
/// worst case, it is a surprise.
///
/// **Intrusive FIFO per level.** Each level holds head and tail pool indices;
/// each order holds `prev` and `next`. Adding appends at the tail, matching
/// consumes from the head, and cancelling unlinks in O(1) from wherever the
/// order happens to be — which matters, because cancels dominate real flow
/// and they do not politely arrive in queue order.
///
/// **Slab pool.** Every order lives in one contiguous allocation made at
/// construction. No allocation happens on the hot path at all; the
/// `NoAllocationDuringSteadyState` test asserts this with a global
/// `operator new` counter over a million mixed operations. The one exception
/// is the overflow map, which allocates by construction — that is why it is a
/// separate, measured path rather than the normal one.
///
/// **Hierarchical bitset.** `best(side)` must not scan. When the touch level
/// empties, the next occupied level is found with at most three
/// `countr_zero`s regardless of how far away it is. On a wide, sparse book a
/// linear scan of the ladder would be the single worst latency event in the
/// engine, and it would hit exactly when the book is thinnest.
///
/// ## Window re-centring
///
/// The window is centred on the first price the book ever sees. If the touch
/// later drifts into the outer quarter of the window, `recentre()` moves it
/// so the touch is central again. The cost is O(window), not O(orders): the
/// level records are moved and the bitset rebuilt, but no order is touched,
/// because orders do not cache their level index (see DD-020 and the comment
/// on `Order`). A cooldown prevents a book straddling an edge from
/// re-centring on every command.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "core/bitset_ladder.hpp"
#include "core/config.hpp"
#include "core/object_pool.hpp"
#include "core/open_hash_map.hpp"
#include "core/types.hpp"

#include "book/book_interface.hpp"

namespace miniex {

/// One price level: aggregate quantity, order count, and the head and tail of
/// its intrusive FIFO.
///
/// Padded to a full cache line when `MINIEX_PAD_PRICE_LEVEL` is on (the
/// default). The padding is a deliberate false-sharing and prefetch choice
/// and it is **not** obviously a win: it makes the resident ladder 16 MB per
/// side instead of 4 MB, which is a lot of cold pages to have around. The
/// comparison is a CMake cache variable precisely so it can be measured both
/// ways rather than asserted; see DD-021.
#if MINIEX_PAD_PRICE_LEVEL
struct alignas(64) PriceLevel {
  AggQty total_qty;          // 8
  std::uint32_t head;        // 4
  std::uint32_t tail;        // 4
  std::uint32_t order_count; // 4
  std::uint32_t _pad[11];    // 44
};
static_assert(sizeof(PriceLevel) == 64);
#else
struct PriceLevel {
  AggQty total_qty;
  std::uint32_t head;
  std::uint32_t tail;
  std::uint32_t order_count;
  std::uint32_t _pad;
};
static_assert(sizeof(PriceLevel) == 24);
#endif

/// FastBook's sizing. At namespace scope rather than nested, so that it can be
/// used as a default argument on the constructor.
struct FastBookConfig {
  std::size_t max_orders{kMaxOrders};
  Price tick{kTickSize};
  /// Centre of the resident window. 0 means "decide on the first add", which
  /// is what makes the book usable for an arbitrary symbol without being told
  /// its price in advance (spec question 25.2).
  Price centre_price{0};
  /// Re-centre when the touch enters the outermost `1/edge_fraction` of the
  /// window at either end. 0 disables re-centring entirely.
  std::size_t edge_fraction{4};
  /// Minimum operations between re-centres, so a book sitting on an edge
  /// cannot re-centre on every command.
  std::uint64_t recentre_cooldown{1U << 16};
};

class FastBook {
public:
  using Config = FastBookConfig;

  explicit FastBook(Config cfg = {})
      : cfg_(cfg), pool_(cfg.max_orders), index_(cfg.max_orders),
        ladder_{std::vector<PriceLevel>(kLadderTicks, empty_level()),
                std::vector<PriceLevel>(kLadderTicks, empty_level())} {
    if (cfg_.tick < 1) cfg_.tick = 1;
    if (cfg_.centre_price > 0) set_base_from_centre(cfg_.centre_price);
  }

  FastBook(const FastBook&) = delete;
  FastBook& operator=(const FastBook&) = delete;

  // -------------------------------------------------------------------------
  // Mutation
  // -------------------------------------------------------------------------

  bool add(OrderId id, Side side, Price price, Qty qty, TsNanos ts,
           ParticipantId participant) {
    if (index_.find(id) != kInvalidIndex) return false;
    if (!have_base_) set_base_from_centre(price);

    const std::uint32_t slot = pool_.allocate();
    if (slot == kInvalidIndex) return false;
    if (!index_.insert(id, slot)) {
      pool_.deallocate(slot);
      return false;
    }

    Order& o = pool_[slot];
    o.price = price;
    o.ts_entry = ts;
    o.id = id;
    o.qty_open = qty;
    o.qty_orig = qty;
    o.next = kInvalidIndex;
    o.prev = kInvalidIndex;
    o.participant = participant;
    o.side = side;
    o.type = OrderType::Limit;
    o.tif = TimeInForce::Day;
    o.flags = kOrderFlagNone;

    PriceLevel& level = level_for_insert(side, price);
    link_at_tail(level, slot);
    level.total_qty += qty;
    ++level.order_count;
    ++live_;
    if (!resident(price)) ++overflow_orders_;

    maybe_recentre();
    return true;
  }

  bool cancel(OrderId id) {
    const std::uint32_t slot = index_.find(id);
    if (slot == kInvalidIndex) return false;
    detach(slot);
    return true;
  }

  bool reduce(OrderId id, Qty by) {
    const std::uint32_t slot = index_.find(id);
    if (slot == kInvalidIndex) return false;
    Order& o = pool_[slot];
    if (by > o.qty_open) return false;
    level_of(o).total_qty -= by;
    o.qty_open -= by;
    if (o.qty_open == 0) detach(slot);
    return true;
  }

  void fill(OrderHandle h, Qty qty) {
    Order& o = *h;
    level_of(o).total_qty -= qty;
    o.qty_open -= qty;
    o.flags |= kOrderFlagTraded;
    if (o.qty_open == 0) detach(slot_of(o));
  }

  void remove(OrderHandle h) { detach(slot_of(*h)); }

  void clear() {
    for (const Side side : {Side::Buy, Side::Sell}) {
      const std::size_t s = index_of(side);
      for (std::size_t i = occupied_[s].find_next_set_at_or_after(0);
           i != Bitset::npos;
           i = occupied_[s].find_next_set_at_or_after(i + 1)) {
        ladder_[s][i] = empty_level();
      }
      occupied_[s].clear();
      overflow_[s].clear();
    }
    pool_.reset();
    index_.clear();
    live_ = 0;
    overflow_orders_ = 0;
    ops_ = 0;
  }

  // -------------------------------------------------------------------------
  // Traversal
  // -------------------------------------------------------------------------

  [[nodiscard]] Price best(Side side) const {
    return better_of(side, ladder_best(side), overflow_best(side));
  }

  [[nodiscard]] Price next_price(Side side, Price after) const {
    return better_of(side, ladder_next(side, after),
                     overflow_next(side, after));
  }

  [[nodiscard]] AggQty qty_at(Side side, Price price) const {
    const PriceLevel* level = find_level(side, price);
    return level == nullptr ? 0 : level->total_qty;
  }

  [[nodiscard]] AggQty qty_at_excluding(Side side, Price price,
                                        ParticipantId ignore) const {
    const PriceLevel* level = find_level(side, price);
    if (level == nullptr) return 0;
    if (ignore == 0) return level->total_qty;
    AggQty total = 0;
    for (std::uint32_t i = level->head; i != kInvalidIndex; i = pool_[i].next)
      if (pool_[i].participant != ignore) total += pool_[i].qty_open;
    return total;
  }

  [[nodiscard]] std::size_t orders_at(Side side, Price price) const {
    const PriceLevel* level = find_level(side, price);
    return level == nullptr ? 0 : level->order_count;
  }

  [[nodiscard]] OrderHandle front(Side side) {
    const Price price = best(side);
    if (price == kInvalidPrice) return nullptr;
    PriceLevel* level = find_level_mutable(side, price);
    if (level == nullptr || level->head == kInvalidIndex) return nullptr;
    return &pool_[level->head];
  }

  [[nodiscard]] ConstOrderHandle find(OrderId id) const {
    const std::uint32_t slot = index_.find(id);
    return slot == kInvalidIndex ? nullptr : &pool_[slot];
  }

  [[nodiscard]] OrderHandle find_mutable(OrderId id) {
    const std::uint32_t slot = index_.find(id);
    return slot == kInvalidIndex ? nullptr : &pool_[slot];
  }

  // -------------------------------------------------------------------------
  // Accounting
  // -------------------------------------------------------------------------

  [[nodiscard]] std::size_t order_count() const { return live_; }
  [[nodiscard]] bool can_add() const { return !pool_.exhausted(); }
  [[nodiscard]] std::size_t level_count(Side side) const {
    return occupied_[index_of(side)].count() + overflow_[index_of(side)].size();
  }

  /// Orders currently resting at a price outside the resident window. The
  /// benchmark publishes this next to the latency numbers, because the
  /// overflow path is the book's known worst case.
  [[nodiscard]] std::size_t overflow_order_count() const {
    return overflow_orders_;
  }
  [[nodiscard]] std::uint64_t recentre_count() const { return recentres_; }
  [[nodiscard]] Price ladder_base() const { return base_; }
  [[nodiscard]] static constexpr std::size_t ladder_width() {
    return kLadderTicks;
  }
  [[nodiscard]] const ObjectPool<Order>& pool() const { return pool_; }

  // -------------------------------------------------------------------------
  // Inspection, for tests and the invariant checker
  // -------------------------------------------------------------------------

  template <class Fn>
  void for_each_level(Side side, Fn&& fn) const {
    for (Price p = best(side); p != kInvalidPrice; p = next_price(side, p)) {
      const PriceLevel* level = find_level(side, p);
      fn(p, level->total_qty, static_cast<std::size_t>(level->order_count));
    }
  }

  template <class Fn>
  void for_each_order(Side side, Price price, Fn&& fn) const {
    const PriceLevel* level = find_level(side, price);
    if (level == nullptr) return;
    for (std::uint32_t i = level->head; i != kInvalidIndex; i = pool_[i].next)
      fn(pool_[i]);
  }

  /// Walks every level and compares the incrementally-maintained aggregates
  /// against a recomputed sum. Debug and test only -- the whole point of
  /// maintaining them incrementally is never to do this on the hot path.
  [[nodiscard]] bool verify_aggregates() const {
    std::size_t counted = 0;
    for (const Side side : {Side::Buy, Side::Sell}) {
      bool ok = true;
      for_each_level(side, [&](Price price, AggQty total, std::size_t n) {
        AggQty walked = 0;
        std::size_t walked_count = 0;
        const PriceLevel* level = find_level(side, price);
        for (std::uint32_t i = level->head; i != kInvalidIndex;
             i = pool_[i].next) {
          walked += pool_[i].qty_open;
          ++walked_count;
          if (pool_[i].price != price || pool_[i].side != side) ok = false;
        }
        if (walked != total || walked_count != n) ok = false;
        counted += walked_count;
      });
      if (!ok) return false;
    }
    return counted == live_ && index_.size() == live_ && pool_.verify() &&
           pool_.live_count() == live_;
  }

private:
  using Bitset = BitsetLadder<kLadderTicks>;
  using OverflowMap = std::map<Price, PriceLevel>;

  static constexpr PriceLevel empty_level() {
    return PriceLevel{0, kInvalidIndex, kInvalidIndex, 0, {}};
  }

  void set_base_from_centre(Price centre) {
    const auto half = static_cast<Price>(kLadderTicks / 2) * cfg_.tick;
    base_ = centre - half;
    have_base_ = true;
  }

  [[nodiscard]] bool resident(Price price) const {
    if (!have_base_) return false;
    if (price < base_) return false;
    const Price offset = price - base_;
    return offset / cfg_.tick < static_cast<Price>(kLadderTicks);
  }

  [[nodiscard]] std::size_t index_for(Price price) const {
    return static_cast<std::size_t>((price - base_) / cfg_.tick);
  }
  [[nodiscard]] Price price_for(std::size_t idx) const {
    return base_ + static_cast<Price>(idx) * cfg_.tick;
  }

  // --- level lookup ---------------------------------------------------------

  [[nodiscard]] const PriceLevel* find_level(Side side, Price price) const {
    const std::size_t s = index_of(side);
    if (resident(price)) {
      const std::size_t i = index_for(price);
      return occupied_[s].test(i) ? &ladder_[s][i] : nullptr;
    }
    const auto it = overflow_[s].find(price);
    return it == overflow_[s].end() ? nullptr : &it->second;
  }

  [[nodiscard]] PriceLevel* find_level_mutable(Side side, Price price) {
    return const_cast<PriceLevel*>(find_level(side, price));
  }

  /// Returns the level for `price`, creating it if it does not exist.
  PriceLevel& level_for_insert(Side side, Price price) {
    const std::size_t s = index_of(side);
    if (resident(price)) {
      const std::size_t i = index_for(price);
      if (!occupied_[s].test(i)) {
        ladder_[s][i] = empty_level();
        occupied_[s].set(i);
      }
      return ladder_[s][i];
    }
    // The overflow path allocates. It is the documented exception to the
    // no-allocation rule, and the reason the ladder-overflow benchmark
    // pattern exists.
    const auto [it, inserted] = overflow_[s].emplace(price, empty_level());
    (void)inserted;
    return it->second;
  }

  [[nodiscard]] PriceLevel& level_of(const Order& o) {
    return *find_level_mutable(o.side, o.price);
  }

  [[nodiscard]] std::uint32_t slot_of(const Order& o) const {
    return static_cast<std::uint32_t>(&o - &pool_[0]);
  }

  // --- intrusive list -------------------------------------------------------

  void link_at_tail(PriceLevel& level, std::uint32_t slot) {
    Order& o = pool_[slot];
    o.prev = level.tail;
    o.next = kInvalidIndex;
    if (level.tail != kInvalidIndex) {
      pool_[level.tail].next = slot;
    } else {
      level.head = slot;
    }
    level.tail = slot;
  }

  void unlink(PriceLevel& level, std::uint32_t slot) {
    Order& o = pool_[slot];
    if (o.prev != kInvalidIndex) {
      pool_[o.prev].next = o.next;
    } else {
      level.head = o.next;
    }
    if (o.next != kInvalidIndex) {
      pool_[o.next].prev = o.prev;
    } else {
      level.tail = o.prev;
    }
    o.prev = kInvalidIndex;
    o.next = kInvalidIndex;
  }

  /// Removes an order from the book entirely: unlink, update aggregates,
  /// retire the level if it is now empty, free the slot.
  void detach(std::uint32_t slot) {
    Order& o = pool_[slot];
    const Side side = o.side;
    const Price price = o.price;
    const std::size_t s = index_of(side);
    const bool is_resident = resident(price);

    PriceLevel* level = is_resident ? &ladder_[s][index_for(price)]
                                    : &overflow_[s].find(price)->second;
    level->total_qty -= o.qty_open;
    --level->order_count;
    unlink(*level, slot);

    if (level->order_count == 0) {
      if (is_resident) {
        occupied_[s].clear(index_for(price));
        *level = empty_level();
      } else {
        overflow_[s].erase(price);
      }
    }
    if (!is_resident) --overflow_orders_;

    (void)index_.erase(o.id);
    pool_.deallocate(slot);
    --live_;
  }

  // --- price search ---------------------------------------------------------

  /// The better (more aggressive) of two prices, either of which may be
  /// kInvalidPrice.
  static Price better_of(Side side, Price a, Price b) {
    if (a == kInvalidPrice) return b;
    if (b == kInvalidPrice) return a;
    return better_price(side, a, b) ? a : b;
  }

  [[nodiscard]] Price ladder_best(Side side) const {
    if (!have_base_) return kInvalidPrice;
    const std::size_t s = index_of(side);
    const std::size_t i =
        side == Side::Buy
            ? occupied_[s].find_prev_set_at_or_before(kLadderTicks - 1)
            : occupied_[s].find_next_set_at_or_after(0);
    return i == Bitset::npos ? kInvalidPrice : price_for(i);
  }

  /// The best ladder price strictly worse than `after`.
  [[nodiscard]] Price ladder_next(Side side, Price after) const {
    if (!have_base_) return kInvalidPrice;
    const std::size_t s = index_of(side);
    if (side == Side::Buy) {
      // Want the highest occupied index below `after`.
      if (after <= base_) return kInvalidPrice;
      std::size_t from = kLadderTicks - 1;
      if (resident(after)) {
        const std::size_t i = index_for(after);
        if (i == 0) return kInvalidPrice;
        from = i - 1;
      } else if (after < base_) {
        return kInvalidPrice;
      }
      const std::size_t found = occupied_[s].find_prev_set_at_or_before(from);
      return found == Bitset::npos ? kInvalidPrice : price_for(found);
    }
    // Sell: want the lowest occupied index above `after`.
    const Price top = price_for(kLadderTicks - 1);
    if (after >= top) return kInvalidPrice;
    const std::size_t from = resident(after) ? index_for(after) + 1 : 0;
    const std::size_t found = occupied_[s].find_next_set_at_or_after(from);
    return found == Bitset::npos ? kInvalidPrice : price_for(found);
  }

  [[nodiscard]] Price overflow_best(Side side) const {
    const OverflowMap& map = overflow_[index_of(side)];
    if (map.empty()) return kInvalidPrice;
    return side == Side::Buy ? map.rbegin()->first : map.begin()->first;
  }

  [[nodiscard]] Price overflow_next(Side side, Price after) const {
    const OverflowMap& map = overflow_[index_of(side)];
    if (map.empty()) return kInvalidPrice;
    if (side == Side::Buy) {
      const auto it = map.lower_bound(after);
      if (it == map.begin()) return kInvalidPrice;
      return std::prev(it)->first;
    }
    const auto it = map.upper_bound(after);
    return it == map.end() ? kInvalidPrice : it->first;
  }

  // --- window re-centring ---------------------------------------------------

  void maybe_recentre() {
    ++ops_;
    if (cfg_.edge_fraction == 0 || !have_base_) return;
    if (ops_ - last_recentre_op_ < cfg_.recentre_cooldown) return;

    const Price bid = best(Side::Buy);
    const Price ask = best(Side::Sell);
    if (bid == kInvalidPrice && ask == kInvalidPrice) return;

    const Price margin =
        static_cast<Price>(kLadderTicks / cfg_.edge_fraction) * cfg_.tick;
    const Price low = base_ + margin;
    const Price high = price_for(kLadderTicks - 1) - margin;

    const bool bid_out = bid != kInvalidPrice && (bid < low || bid > high);
    const bool ask_out = ask != kInvalidPrice && (ask < low || ask > high);
    if (!bid_out && !ask_out) return;

    Price centre;
    if (bid != kInvalidPrice && ask != kInvalidPrice)
      centre = bid + (ask - bid) / 2;
    else
      centre = bid != kInvalidPrice ? bid : ask;
    recentre(centre);
  }

public:
  /// Moves the resident window so that `centre` sits in the middle.
  ///
  /// Cost is O(window): the occupied level records are relocated and the
  /// bitset is rebuilt by a single pass over the window. **No order record is
  /// touched**, which is only possible because orders do not cache their
  /// level index — see the comment on `Order` and DD-020. With a cached index
  /// this would be O(live orders), i.e. up to four million writes.
  void recentre(Price centre) {
    if (!have_base_) {
      set_base_from_centre(centre);
      return;
    }
    const Price old_base = base_;
    set_base_from_centre(centre);
    if (base_ == old_base) return;
    ++recentres_;
    last_recentre_op_ = ops_;

    for (const Side side : {Side::Buy, Side::Sell}) {
      const std::size_t s = index_of(side);
      std::vector<PriceLevel>& array = ladder_[s];
      OverflowMap& overflow = overflow_[s];

      // 1. Everything currently resident that will fall outside the new
      //    window moves to the overflow map, and everything that stays is
      //    collected with its price so it can be replaced at its new index.
      OverflowMap staying;
      for (std::size_t i = occupied_[s].find_next_set_at_or_after(0);
           i != Bitset::npos;
           i = occupied_[s].find_next_set_at_or_after(i + 1)) {
        const Price price = old_base + static_cast<Price>(i) * cfg_.tick;
        if (resident(price)) {
          staying.emplace(price, array[i]);
        } else {
          overflow.emplace(price, array[i]);
          overflow_orders_ += array[i].order_count;
        }
        array[i] = empty_level();
      }
      occupied_[s].clear();

      // 2. Overflow entries that the new window covers move back in.
      for (auto it = overflow.begin(); it != overflow.end();) {
        if (resident(it->first)) {
          staying.emplace(it->first, it->second);
          overflow_orders_ -= it->second.order_count;
          it = overflow.erase(it);
        } else {
          ++it;
        }
      }

      // 3. Place everything that is resident at its new index.
      for (const auto& [price, level] : staying) {
        const std::size_t i = index_for(price);
        array[i] = level;
        occupied_[s].set(i);
      }
    }
  }

private:
  Config cfg_;
  ObjectPool<Order> pool_;
  OpenHashMap index_;
  std::vector<PriceLevel> ladder_[2];
  Bitset occupied_[2];
  OverflowMap overflow_[2];

  Price base_{0};
  bool have_base_{false};
  std::size_t live_{0};
  std::size_t overflow_orders_{0};
  std::uint64_t ops_{0};
  std::uint64_t last_recentre_op_{0};
  std::uint64_t recentres_{0};
};

static_assert(OrderBook<FastBook>);

} // namespace miniex
