/// \file messages.hpp
/// \brief Inbound commands and outbound events.
///
/// Both directions are fixed-size PODs. The inbound side goes through the
/// SPSC ingress ring; the outbound side goes to a templated sink and, when
/// the tap is compiled in, into shared memory. Neither ever allocates.
///
/// **Why a tagged union and not `std::variant`.** `std::variant` stores its
/// discriminant after the payload with its own alignment rules, its
/// `visit` is a function-pointer table that the optimiser frequently fails
/// to devirtualise, and it is not trivially copyable unless every
/// alternative is. A plain discriminant plus a union is 48 bytes, memcpy-able
/// straight into the ring, and switches to a jump table. The claim that
/// variant is slower here is a measurement in `bench/micro`, not an
/// assertion -- see docs/design-decisions.md DD-012.
///
/// **Why one Event type and not seven.** Every outbound event is 64 bytes
/// with a type tag, rather than a per-type struct hierarchy. This makes the
/// sink interface one function, makes the canonical text format one
/// switch, and makes the tap record a straight copy. The cost is a few
/// unused fields per event, which is free: the record is one cache line
/// either way.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "core/types.hpp"

namespace miniex {

// ---------------------------------------------------------------------------
// Inbound
// ---------------------------------------------------------------------------

struct NewOrder {
  OrderId id;
  Price price; ///< ignored when type == Market
  Qty qty;
  SymbolId symbol;
  ParticipantId participant;
  Side side;
  OrderType type;
  TimeInForce tif;
  std::uint8_t _pad;
};
static_assert(sizeof(NewOrder) == 32);
static_assert(std::is_trivially_copyable_v<NewOrder>);

struct CancelOrder {
  OrderId id;
};

/// `new_qty` is the new *open* (working) quantity, not a delta and not the
/// original quantity. This matches ITCH's Order Replace, which carries the
/// new share count for the replacement order, and it is the definition the
/// Python oracle implements.
struct ReplaceOrder {
  OrderId id;
  Price new_price;
  Qty new_qty;
};

enum class CommandType : std::uint8_t {
  New = 0,
  Cancel = 1,
  Replace = 2
};

/// The unit pushed through the ingress ring.
///
/// The timestamp travels *with* the command and is assigned by whoever
/// produced it (the feed handler, the synthetic generator, the harness). The
/// engine never reads a clock: a clock read inside `on_new` would make the
/// event stream depend on wall time, which destroys both determinism and the
/// ability to replay a recorded run and get the same answer.
struct Command {
  CommandType type;
  std::uint8_t _pad[7];
  TsNanos ts;
  union Body {
    NewOrder neworder;
    CancelOrder cancel;
    ReplaceOrder replace;
  } body;

  static Command make_new(TsNanos ts, const NewOrder& n) noexcept {
    Command c{};
    c.type = CommandType::New;
    c.ts = ts;
    c.body.neworder = n;
    return c;
  }
  static Command make_cancel(TsNanos ts, OrderId id) noexcept {
    Command c{};
    c.type = CommandType::Cancel;
    c.ts = ts;
    c.body.cancel = CancelOrder{id};
    return c;
  }
  static Command make_replace(TsNanos ts, OrderId id, Price price,
                              Qty qty) noexcept {
    Command c{};
    c.type = CommandType::Replace;
    c.ts = ts;
    c.body.replace = ReplaceOrder{id, price, qty};
    return c;
  }
};
static_assert(sizeof(Command) == 48);
static_assert(std::is_trivially_copyable_v<Command>);

// ---------------------------------------------------------------------------
// Outbound
// ---------------------------------------------------------------------------

enum class EventType : std::uint8_t {
  Ack = 0,   ///< command accepted; the order now exists in the engine
  Reject,    ///< command refused; `reason` says why, nothing changed
  Fill,      ///< one side's view of a match (emitted per participant)
  Cancelled, ///< an order left the book; `qty` is what was still open
  Replaced,  ///< a replace was applied; `price`/`qty` are the new values
  Trade,     ///< the public print: aggressor, resting, price, quantity
  BookDelta, ///< a level's aggregate changed (tap/UI only)
};

constexpr const char* to_string(EventType t) noexcept {
  switch (t) {
  case EventType::Ack: return "ACK";
  case EventType::Reject: return "REJECT";
  case EventType::Fill: return "FILL";
  case EventType::Cancelled: return "CANCELLED";
  case EventType::Replaced: return "REPLACED";
  case EventType::Trade: return "TRADE";
  case EventType::BookDelta: return "DELTA";
  }
  return "UNKNOWN";
}

/// Bit flags on an outbound event.
enum EventFlags : std::uint8_t {
  kEventFlagNone = 0,
  /// The order this event concerns was a market order. Set on its Ack so
  /// that a consumer of the stream alone -- the invariant ledger, the
  /// dashboard -- can tell "no limit price" from "limit price zero" without
  /// having to have seen the original command.
  kEventFlagMarketOrder = 1U << 0,
};

/// One cache line, trivially copyable, no pointers: the same record goes to
/// the test sink, the canonical text format, and the shared-memory tap.
struct Event {
  SeqNum seq;         ///< monotonic, assigned by the engine; determinism key
  TsNanos ts;         ///< copied from the command that caused it
  OrderId id;         ///< subject order (the aggressor, for a Trade)
  OrderId counter_id; ///< the resting order, for Trade/Fill; 0 otherwise
  MatchId match_id;   ///< monotonic trade id; 0 for non-trade events
  Price price;
  Qty qty;           ///< quantity this event is about
  Qty qty_remaining; ///< open quantity left on `id` after this event
  SymbolId symbol;
  ParticipantId participant;
  EventType type;
  Side side;           ///< the aggressor's side, for a Trade
  RejectReason reason; ///< Reject only
  std::uint8_t flags;
};
static_assert(sizeof(Event) == 64, "an event must fit one cache line");
static_assert(std::is_trivially_copyable_v<Event>);

} // namespace miniex
