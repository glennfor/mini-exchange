/// \file types.hpp
/// \brief Fundamental value types shared by every layer of the exchange.
///
/// This header is the one place where the numeric conventions of the system
/// are fixed. It depends on nothing else in the repository.
///
/// Conventions (see docs/design-decisions.md):
///   * Prices are integer ticks at 1e-4 dollars, matching both Nasdaq ITCH
///     and LOBSTER. $100.2100 is 1002100. Floating point is forbidden in
///     core/, book/, engine/ and feed/ -- a price that cannot be compared
///     exactly is a price that produces non-deterministic matching.
///   * Quantities are shares, never lots.
///   * Pool indices are 32-bit, not pointers: half the size, relocatable,
///     and directly serialisable into the observability tap.
#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

namespace miniex {

// ---------------------------------------------------------------------------
// Scalars
// ---------------------------------------------------------------------------

/// Price in integer ticks of 1e-4 dollars. Signed so that price arithmetic
/// (offsets from a ladder base, spread computations) cannot silently wrap.
using Price = std::int64_t;

/// Order quantity in shares.
using Qty = std::uint32_t;

/// Aggregate quantity at a price level. Widened because a deep level in a
/// liquid name can exceed 2^32 shares over a full day of adds.
using AggQty = std::uint64_t;

/// Client order id on the engine side; ITCH order reference number on the
/// feed side. Never reused within a run.
using OrderId = std::uint64_t;

/// Monotonic per-run outbound event sequence, assigned by the engine.
/// Determinism is defined over this stream.
using SeqNum = std::uint64_t;

/// Monotonic trade identifier.
using MatchId = std::uint64_t;

/// Dense symbol index. The ITCH "stock locate" field maps directly onto it.
/// Hot-path structures are never keyed by string.
using SymbolId = std::uint16_t;

/// Participant identifier, used by self-trade prevention.
using ParticipantId = std::uint16_t;

/// Nanoseconds. Two distinct meanings, documented at every use site:
///   * market data: nanoseconds since midnight US/Eastern (ITCH convention);
///   * engine measurement: nanoseconds since an arbitrary steady epoch.
using TsNanos = std::int64_t;

/// The scale factor between Price units and dollars.
inline constexpr Price kPriceScale = 10000;

// ---------------------------------------------------------------------------
// Price sentinels
// ---------------------------------------------------------------------------

/// "No price" -- an empty book side reports this from best().
inline constexpr Price kInvalidPrice = std::numeric_limits<Price>::min();

/// A market buy is a limit order at +infinity: it is willing to cross any
/// ask, so making it the maximum representable price lets the matching loop
/// use the same "is my limit still good?" comparison for every order type.
inline constexpr Price kMarketBuyPrice = std::numeric_limits<Price>::max();

/// A market sell is a limit order at -infinity. It cannot be
/// numeric_limits<Price>::min() because that value is kInvalidPrice, so it is
/// one tick above it; no real price is ever anywhere near either value.
inline constexpr Price kMarketSellPrice = std::numeric_limits<Price>::min() + 1;

static_assert(kMarketSellPrice != kInvalidPrice);
static_assert(kMarketSellPrice < kMarketBuyPrice);

/// Sentinel for an unset 32-bit pool index. Index 0 is a valid order.
inline constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

/// Sentinel for an unset price-level index.
inline constexpr std::uint32_t kInvalidLevel = kInvalidIndex;

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

/// Book side. The underlying values are 0 and 1 so that Side can index a
/// two-element array without a branch.
enum class Side : std::uint8_t {
  Buy = 0,
  Sell = 1
};

/// The opposite side, by bit flip -- no branch, no table.
constexpr Side opposite(Side s) noexcept {
  return static_cast<Side>(static_cast<std::uint8_t>(s) ^ 1U);
}

/// Side as an array index.
constexpr std::size_t index_of(Side s) noexcept {
  return static_cast<std::size_t>(s);
}

/// 'B' / 'S', for human-readable output and the wire protocol.
constexpr char side_char(Side s) noexcept {
  return s == Side::Buy ? 'B' : 'S';
}

enum class OrderType : std::uint8_t {
  Limit = 0,
  Market = 1
};

/// PostOnly is modelled as a time-in-force rather than an order type: it
/// constrains *when the order may rest*, exactly as IOC and FOK constrain how
/// long it may live, and keeping it out of OrderType means the type field
/// stays a pure statement about price (limit vs market).
enum class TimeInForce : std::uint8_t {
  Day = 0,
  IOC = 1,
  FOK = 2,
  PostOnly = 3
};

/// Exhaustive list of the reasons the engine refuses a command. Every reject
/// path maps to exactly one of these; there is no generic "error".
enum class RejectReason : std::uint8_t {
  None = 0,
  DuplicateOrderId,
  UnknownOrder,
  TooLateToCancel,
  ZeroQuantity,
  InvalidPrice,
  PriceOutOfBand,
  PoolExhausted,
  FokUnfillable,
  PostOnlyWouldCross,
  MarketOrderNotFillable,
  MarketDayNotSupported,
  SelfTradePrevented,
};

/// Stable, greppable names for RejectReason, used by the canonical event text
/// format that the C++ engine and the Python oracle both emit.
constexpr const char* to_string(RejectReason r) noexcept {
  switch (r) {
  case RejectReason::None: return "None";
  case RejectReason::DuplicateOrderId: return "DuplicateOrderId";
  case RejectReason::UnknownOrder: return "UnknownOrder";
  case RejectReason::TooLateToCancel: return "TooLateToCancel";
  case RejectReason::ZeroQuantity: return "ZeroQuantity";
  case RejectReason::InvalidPrice: return "InvalidPrice";
  case RejectReason::PriceOutOfBand: return "PriceOutOfBand";
  case RejectReason::PoolExhausted: return "PoolExhausted";
  case RejectReason::FokUnfillable: return "FokUnfillable";
  case RejectReason::PostOnlyWouldCross: return "PostOnlyWouldCross";
  case RejectReason::MarketOrderNotFillable: return "MarketOrderNotFillable";
  case RejectReason::MarketDayNotSupported: return "MarketDayNotSupported";
  case RejectReason::SelfTradePrevented: return "SelfTradePrevented";
  }
  return "Unknown";
}

constexpr const char* to_string(OrderType t) noexcept {
  return t == OrderType::Limit ? "Limit" : "Market";
}

constexpr const char* to_string(TimeInForce t) noexcept {
  switch (t) {
  case TimeInForce::Day: return "Day";
  case TimeInForce::IOC: return "IOC";
  case TimeInForce::FOK: return "FOK";
  case TimeInForce::PostOnly: return "PostOnly";
  }
  return "Unknown";
}

// ---------------------------------------------------------------------------
// Price comparison
// ---------------------------------------------------------------------------

/// True if \p a is a strictly more aggressive price than \p b for \p side:
/// higher is better for a bid, lower is better for an ask.
constexpr bool better_price(Side side, Price a, Price b) noexcept {
  return side == Side::Buy ? (a > b) : (a < b);
}

/// True if an order on \p side with limit \p limit is willing to trade
/// against a resting order priced at \p resting.
constexpr bool crosses(Side side, Price limit, Price resting) noexcept {
  return side == Side::Buy ? (limit >= resting) : (limit <= resting);
}

// ---------------------------------------------------------------------------
// Order record
// ---------------------------------------------------------------------------

/// Bit flags carried on an order.
enum OrderFlags : std::uint8_t {
  kOrderFlagNone = 0,
  /// Set once the order has been (partially or fully) executed; used to
  /// distinguish a cancel-too-late from a cancel of an untouched order.
  kOrderFlagTraded = 1U << 0,
};

/// The resting-order record. Exactly one cache line, because the matching
/// loop touches one of these per fill and the cancel path touches exactly
/// one. The layout is asserted so that a future field addition fails the
/// build rather than silently doubling the cost of a cancel.
struct alignas(64) Order {
  Price price;               ///< limit price in ticks
  TsNanos ts_entry;          ///< engine acceptance order (see docs)
  OrderId id;                ///< client order id
  Qty qty_open;              ///< shares still working
  Qty qty_orig;              ///< shares on arrival
  std::uint32_t next;        ///< intrusive FIFO successor (pool index)
  std::uint32_t prev;        ///< intrusive FIFO predecessor (pool index)
  std::uint32_t level;       ///< owning price-level index
  ParticipantId participant; ///< for self-trade prevention
  Side side;
  OrderType type;
  TimeInForce tif;
  std::uint8_t flags;
  std::uint8_t _pad[14];
};

static_assert(sizeof(Order) == 64, "Order must occupy exactly one cache line");
static_assert(alignof(Order) == 64);
static_assert(std::is_trivially_copyable_v<Order>);

// ---------------------------------------------------------------------------
// Static guarantees relied upon elsewhere
// ---------------------------------------------------------------------------

static_assert(sizeof(Price) == 8);
static_assert(sizeof(Qty) == 4);
static_assert(sizeof(OrderId) == 8);
static_assert(sizeof(SymbolId) == 2);
static_assert(std::is_signed_v<Price>, "price offsets must not wrap");
static_assert(!std::is_signed_v<Qty>);
static_assert(opposite(Side::Buy) == Side::Sell);
static_assert(opposite(Side::Sell) == Side::Buy);
static_assert(opposite(opposite(Side::Buy)) == Side::Buy);
static_assert(opposite(opposite(Side::Sell)) == Side::Sell);
static_assert(index_of(Side::Buy) == 0 && index_of(Side::Sell) == 1);

} // namespace miniex
