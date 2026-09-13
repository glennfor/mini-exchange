/// \file normalizer.hpp
/// \brief ITCH messages to the one event type the rest of the system consumes.
///
/// Both consumers -- the reconstruction book (M7) and the order-flow injector
/// (M8) -- take `NormalizedEvent`, so neither of them ever sees an ITCH byte
/// layout. That boundary is what keeps `book/` free of any knowledge of the
/// feed, and it is what would make a second feed format (a different
/// exchange, a different protocol version) a new normaliser rather than a new
/// book.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "core/types.hpp"

#include "feed/itch50.hpp"

namespace miniex {

enum class MdAction : std::uint8_t {
  Add,
  Execute,
  ExecuteWithPrice,
  Cancel,
  Delete,
  Replace,
  Trade,
  Cross,
  Broken,
  TradingAction,
  SystemEvent,
  /// Decoded and counted, but of no interest to the book.
  Other,
};

constexpr const char* to_string(MdAction a) noexcept {
  switch (a) {
  case MdAction::Add: return "Add";
  case MdAction::Execute: return "Execute";
  case MdAction::ExecuteWithPrice: return "ExecuteWithPrice";
  case MdAction::Cancel: return "Cancel";
  case MdAction::Delete: return "Delete";
  case MdAction::Replace: return "Replace";
  case MdAction::Trade: return "Trade";
  case MdAction::Cross: return "Cross";
  case MdAction::Broken: return "Broken";
  case MdAction::TradingAction: return "TradingAction";
  case MdAction::SystemEvent: return "SystemEvent";
  case MdAction::Other: return "Other";
  }
  return "Unknown";
}

/// The decoder's output. Trivially copyable and fixed size, so it can go
/// through a ring or be written to a file without translation.
struct NormalizedEvent {
  TsNanos ts{0};
  OrderId ref{0};     ///< ITCH order reference number
  OrderId new_ref{0}; ///< Replace only
  Price price{0};
  Qty qty{0};
  MatchId match{0};
  SymbolId symbol{0}; ///< the ITCH stock locate, used directly
  MdAction action{MdAction::Other};
  Side side{Side::Buy};
  /// 'C' messages carry a printable flag: a non-printable execution still
  /// changes the book but is excluded from volume and last-price statistics.
  bool printable{true};
  char detail{'\0'}; ///< event code / trading state / cross type
};
static_assert(sizeof(NormalizedEvent) == 56);
static_assert(std::is_trivially_copyable_v<NormalizedEvent>);

/// Converts one decoded ITCH message. Returns false only when the message
/// type is one this project does not normalise (it has still been decoded and
/// counted by then).
inline bool normalize(const std::byte* p, char type,
                      NormalizedEvent& out) noexcept {
  // Zero the whole record, padding included, rather than assigning a
  // value-initialised temporary. Padding bytes left indeterminate make the
  // struct unsafe to `memcmp` and unsafe to write to a file or a shared-memory
  // ring, and the resulting bugs are order-dependent and maddening -- the
  // dispatch-equivalence test caught exactly that, passing in isolation and
  // failing in a full run.
  std::memset(static_cast<void*>(&out), 0, sizeof(out));
  out.printable = true;
  out.side = Side::Buy;
  out.action = MdAction::Other;
  switch (type) {
  case 'A':
  case 'F': {
    ItchAddOrder msg{};
    if (!decode_add_order(p, msg)) return false;
    out.ts = msg.header.timestamp;
    out.symbol = msg.header.stock_locate;
    out.ref = msg.order_ref;
    out.price = msg.price;
    out.qty = msg.shares;
    out.side = msg.side;
    out.action = MdAction::Add;
    return true;
  }
  case 'E':
  case 'C': {
    ItchOrderExecuted msg{};
    if (!decode_order_executed(p, msg)) return false;
    out.ts = msg.header.timestamp;
    out.symbol = msg.header.stock_locate;
    out.ref = msg.order_ref;
    out.qty = msg.executed_shares;
    out.match = msg.match_number;
    out.price = msg.price;
    out.printable = msg.printable;
    out.action = msg.has_price ? MdAction::ExecuteWithPrice : MdAction::Execute;
    return true;
  }
  case 'X':
  case 'D': {
    ItchOrderCancel msg{};
    if (!decode_order_cancel(p, msg)) return false;
    out.ts = msg.header.timestamp;
    out.symbol = msg.header.stock_locate;
    out.ref = msg.order_ref;
    out.qty = msg.cancelled_shares;
    out.action = msg.is_delete ? MdAction::Delete : MdAction::Cancel;
    return true;
  }
  case 'U': {
    ItchOrderReplace msg{};
    if (!decode_order_replace(p, msg)) return false;
    out.ts = msg.header.timestamp;
    out.symbol = msg.header.stock_locate;
    out.ref = msg.original_ref;
    out.new_ref = msg.new_ref;
    out.qty = msg.shares;
    out.price = msg.price;
    out.action = MdAction::Replace;
    return true;
  }
  case 'P': {
    ItchTrade msg{};
    if (!decode_trade(p, msg)) return false;
    out.ts = msg.header.timestamp;
    out.symbol = msg.header.stock_locate;
    out.ref = msg.order_ref;
    out.qty = msg.shares;
    out.price = msg.price;
    out.match = msg.match_number;
    out.side = msg.side;
    out.action = MdAction::Trade;
    return true;
  }
  case 'Q': {
    ItchTrade msg{};
    if (!decode_cross_trade(p, msg)) return false;
    out.ts = msg.header.timestamp;
    out.symbol = msg.header.stock_locate;
    out.qty = msg.shares;
    out.price = msg.price;
    out.match = msg.match_number;
    out.detail = msg.cross_type;
    out.action = MdAction::Cross;
    return true;
  }
  case 'B': {
    ItchBrokenTrade msg{};
    if (!decode_broken_trade(p, msg)) return false;
    out.ts = msg.header.timestamp;
    out.symbol = msg.header.stock_locate;
    out.match = msg.match_number;
    out.action = MdAction::Broken;
    return true;
  }
  case 'H': {
    ItchTradingAction msg{};
    if (!decode_trading_action(p, msg)) return false;
    out.ts = msg.header.timestamp;
    out.symbol = msg.header.stock_locate;
    out.detail = msg.state;
    out.action = MdAction::TradingAction;
    return true;
  }
  case 'S': {
    ItchSystemEvent msg{};
    if (!decode_system_event(p, msg)) return false;
    out.ts = msg.header.timestamp;
    out.symbol = msg.header.stock_locate;
    out.detail = msg.event_code;
    out.action = MdAction::SystemEvent;
    return true;
  }
  default: {
    const ItchHeader h = decode_header(p);
    out.ts = h.timestamp;
    out.symbol = h.stock_locate;
    out.action = MdAction::Other;
    return true;
  }
  }
}

} // namespace miniex
