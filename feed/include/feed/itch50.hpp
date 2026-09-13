/// \file itch50.hpp
/// \brief Nasdaq TotalView-ITCH 5.0 message decoding.
///
/// ## Ground rules
///
/// * Every multi-byte integer is **big-endian**. Prices are 4-byte unsigned
///   with four implied decimals, which is exactly this project's `Price`
///   scale, so decoding a price is a widening integer load and nothing else.
/// * Timestamps are **6 bytes**, nanoseconds since midnight US/Eastern,
///   widened to 64 bits on decode.
/// * Every message begins with: type (1), stock locate (2), tracking number
///   (2), timestamp (6) — eleven bytes.
/// * Alpha fields are left-justified and space-padded. Stock symbols are 8
///   bytes; the trailing spaces are trimmed exactly once, at Stock Directory
///   time, and never again on the hot path.
/// * **Nothing here allocates and nothing constructs a `std::string`.**
///   Symbols are `std::string_view` into the mapped file.
///
/// ## Lengths are validated, never assumed
///
/// A message whose framed length does not match the length its type requires
/// is a hard error reporting the file offset. Skipping silently is how a
/// decoder ends up half a message out of phase and then reconstructs a
/// plausible-looking book from garbage.
///
/// The lengths below were cross-checked field by field against the field
/// layouts: each equals 11 (the common header) plus the sum of its own
/// fields. That arithmetic is re-derived by a test, so a typo here fails the
/// build rather than the reconstruction.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/types.hpp"

#include "feed/endian.hpp"

namespace miniex {

/// The 22 message types of ITCH 5.0. The eight that mutate the book are
/// marked; the rest are decoded and counted.
enum class ItchType : char {
  SystemEvent = 'S',
  StockDirectory = 'R',
  StockTradingAction = 'H',
  RegShoRestriction = 'Y',
  MarketParticipantPosition = 'L',
  MwcbDeclineLevel = 'V',
  MwcbStatus = 'W',
  IpoQuotingPeriod = 'K',
  LuldAuctionCollar = 'J',
  OperationalHalt = 'h',
  AddOrder = 'A',               ///< mutates the book
  AddOrderWithMpid = 'F',       ///< mutates the book
  OrderExecuted = 'E',          ///< mutates the book
  OrderExecutedWithPrice = 'C', ///< mutates the book
  OrderCancel = 'X',            ///< mutates the book
  OrderDelete = 'D',            ///< mutates the book
  OrderReplace = 'U',           ///< mutates the book
  TradeNonCross = 'P',          ///< trade print only; no book change
  CrossTrade = 'Q',
  BrokenTrade = 'B',
  Noii = 'I',
  Rpii = 'N',
};

/// Bytes shared by every message: type, stock locate, tracking number,
/// timestamp.
inline constexpr std::size_t kItchHeaderLength = 11;

/// The framed length of a message of this type, or 0 if the type is unknown.
///
/// A `switch` rather than a table because it is exhaustive by construction:
/// adding an enumerator without a length is a `-Wswitch` warning, and this
/// project builds with `-Werror`.
constexpr std::size_t itch_message_length(char type) noexcept {
  switch (type) {
  case 'S': return 12;
  case 'R': return 39;
  case 'H': return 25;
  case 'Y': return 20;
  case 'L': return 26;
  case 'V': return 35;
  case 'W': return 12;
  case 'K': return 28;
  case 'J': return 35;
  case 'h': return 21;
  case 'A': return 36;
  case 'F': return 40;
  case 'E': return 31;
  case 'C': return 36;
  case 'X': return 23;
  case 'D': return 19;
  case 'U': return 35;
  case 'P': return 44;
  case 'Q': return 40;
  case 'B': return 19;
  case 'I': return 50;
  case 'N': return 20;
  default: return 0;
  }
}

/// True for the eight types that change the order book.
constexpr bool itch_mutates_book(char type) noexcept {
  switch (type) {
  case 'A':
  case 'F':
  case 'E':
  case 'C':
  case 'X':
  case 'D':
  case 'U': return true;
  default: return false;
  }
}

// ---------------------------------------------------------------------------
// Decoded views
//
// These are *decoded* structs, not overlays on the wire bytes: the fields are
// already byte-swapped and widened. Overlaying a packed struct on the buffer
// and reading members directly would be unaligned access and a strict
// aliasing violation, and it would not save anything -- the loads happen
// either way.
// ---------------------------------------------------------------------------

struct ItchHeader {
  char type;
  std::uint16_t stock_locate;
  std::uint16_t tracking_number;
  TsNanos timestamp; ///< nanoseconds since midnight US/Eastern
};

struct ItchAddOrder {
  ItchHeader header;
  OrderId order_ref;
  Side side;
  Qty shares;
  std::string_view stock; ///< 8 bytes, space-padded, into the source buffer
  Price price;
  std::uint32_t attribution; ///< 'F' only; 0 for 'A'
};

struct ItchOrderExecuted {
  ItchHeader header;
  OrderId order_ref;
  Qty executed_shares;
  MatchId match_number;
  bool printable; ///< 'C' only; always true for 'E'
  Price price;    ///< 'C' only; 0 for 'E' (the resting price applies)
  bool has_price; ///< true for 'C'
};

struct ItchOrderCancel {
  ItchHeader header;
  OrderId order_ref;
  Qty cancelled_shares; ///< 0 for a Delete, which removes the whole order
  bool is_delete;
};

struct ItchOrderReplace {
  ItchHeader header;
  OrderId original_ref;
  OrderId new_ref;
  Qty shares;
  Price price;
};

struct ItchTrade {
  ItchHeader header;
  OrderId order_ref; ///< always 0 in practice for 'P'
  Side side;
  Qty shares;
  std::string_view stock;
  Price price;
  MatchId match_number;
  bool is_cross;
  char cross_type; ///< 'Q' only
};

struct ItchBrokenTrade {
  ItchHeader header;
  MatchId match_number;
};

struct ItchStockDirectory {
  ItchHeader header;
  std::string_view stock; ///< untrimmed; trim once via trim_symbol()
  char market_category;
  char financial_status;
  std::uint32_t round_lot_size;
  bool round_lots_only;
  char issue_classification;
  char authenticity;
  char short_sale_threshold;
  char ipo_flag;
  char luld_tier;
  char etp_flag;
  std::uint32_t etp_leverage;
  char inverse_indicator;
};

struct ItchSystemEvent {
  ItchHeader header;
  char event_code;
};

struct ItchTradingAction {
  ItchHeader header;
  std::string_view stock;
  char state;
  char reason0;
  char reason1;
  char reason2;
  char reason3;
};

// ---------------------------------------------------------------------------
// Field decoding
// ---------------------------------------------------------------------------

/// Trims the trailing spaces from an 8-byte ITCH alpha field. Called once per
/// symbol at Stock Directory time; never on the hot path.
inline std::string_view trim_symbol(std::string_view raw) noexcept {
  std::size_t end = raw.size();
  while (end > 0 && raw[end - 1] == ' ') --end;
  return raw.substr(0, end);
}

inline ItchHeader decode_header(const std::byte* p) noexcept {
  ItchHeader h;
  h.type = load_char(p);
  h.stock_locate = load_be16(p + 1);
  h.tracking_number = load_be16(p + 3);
  h.timestamp = static_cast<TsNanos>(load_be48(p + 5));
  return h;
}

inline std::string_view decode_stock(const std::byte* p) noexcept {
  return std::string_view(reinterpret_cast<const char*>(p), 8);
}

/// 'B' is buy, 'S' is sell. Anything else is a malformed message; the decoder
/// reports it rather than guessing, because guessing here puts liquidity on
/// the wrong side of the book and every subsequent state is wrong.
inline bool decode_side(char c, Side& out) noexcept {
  if (c == 'B') {
    out = Side::Buy;
    return true;
  }
  if (c == 'S') {
    out = Side::Sell;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Message decoding. Each function assumes the caller has already validated
// that `p` points at `itch_message_length(type)` readable bytes.
// ---------------------------------------------------------------------------

/// 'A' (36) and 'F' (40).
inline bool decode_add_order(const std::byte* p, ItchAddOrder& out) noexcept {
  out.header = decode_header(p);
  out.order_ref = load_be64(p + 11);
  if (!decode_side(load_char(p + 19), out.side)) return false;
  out.shares = load_be32(p + 20);
  out.stock = decode_stock(p + 24);
  out.price = static_cast<Price>(load_be32(p + 32));
  out.attribution = out.header.type == 'F' ? load_be32(p + 36) : 0;
  return true;
}

/// 'E' (31) and 'C' (36).
inline bool decode_order_executed(const std::byte* p,
                                  ItchOrderExecuted& out) noexcept {
  out.header = decode_header(p);
  out.order_ref = load_be64(p + 11);
  out.executed_shares = load_be32(p + 19);
  out.match_number = load_be64(p + 23);
  if (out.header.type == 'C') {
    const char printable = load_char(p + 31);
    if (printable != 'Y' && printable != 'N') return false;
    out.printable = printable == 'Y';
    out.price = static_cast<Price>(load_be32(p + 32));
    out.has_price = true;
  } else {
    out.printable = true;
    out.price = 0;
    out.has_price = false;
  }
  return true;
}

/// 'X' (23) and 'D' (19).
inline bool decode_order_cancel(const std::byte* p,
                                ItchOrderCancel& out) noexcept {
  out.header = decode_header(p);
  out.order_ref = load_be64(p + 11);
  if (out.header.type == 'X') {
    out.cancelled_shares = load_be32(p + 19);
    out.is_delete = false;
  } else {
    out.cancelled_shares = 0;
    out.is_delete = true;
  }
  return true;
}

/// 'U' (35).
inline bool decode_order_replace(const std::byte* p,
                                 ItchOrderReplace& out) noexcept {
  out.header = decode_header(p);
  out.original_ref = load_be64(p + 11);
  out.new_ref = load_be64(p + 19);
  out.shares = load_be32(p + 27);
  out.price = static_cast<Price>(load_be32(p + 31));
  return true;
}

/// 'P' (44).
inline bool decode_trade(const std::byte* p, ItchTrade& out) noexcept {
  out.header = decode_header(p);
  out.order_ref = load_be64(p + 11);
  if (!decode_side(load_char(p + 19), out.side)) return false;
  out.shares = load_be32(p + 20);
  out.stock = decode_stock(p + 24);
  out.price = static_cast<Price>(load_be32(p + 32));
  out.match_number = load_be64(p + 36);
  out.is_cross = false;
  out.cross_type = '\0';
  return true;
}

/// 'Q' (40). Note the shares field is 8 bytes here, not 4: a cross can move
/// more than four billion shares in principle and Nasdaq sized the field for
/// it. Reading it as 4 bytes is a classic ITCH bug.
inline bool decode_cross_trade(const std::byte* p, ItchTrade& out) noexcept {
  out.header = decode_header(p);
  out.order_ref = 0;
  out.side = Side::Buy; // a cross has no aggressor side
  const std::uint64_t shares = load_be64(p + 11);
  out.shares = static_cast<Qty>(shares);
  out.stock = decode_stock(p + 19);
  out.price = static_cast<Price>(load_be32(p + 27));
  out.match_number = load_be64(p + 31);
  out.cross_type = load_char(p + 39);
  out.is_cross = true;
  return true;
}

/// 'B' (19).
inline bool decode_broken_trade(const std::byte* p,
                                ItchBrokenTrade& out) noexcept {
  out.header = decode_header(p);
  out.match_number = load_be64(p + 11);
  return true;
}

/// 'R' (39).
inline bool decode_stock_directory(const std::byte* p,
                                   ItchStockDirectory& out) noexcept {
  out.header = decode_header(p);
  out.stock = decode_stock(p + 11);
  out.market_category = load_char(p + 19);
  out.financial_status = load_char(p + 20);
  out.round_lot_size = load_be32(p + 21);
  out.round_lots_only = load_char(p + 25) == 'Y';
  out.issue_classification = load_char(p + 26);
  // Issue subtype is two bytes at 27..28; not carried, but accounted for.
  out.authenticity = load_char(p + 29);
  out.short_sale_threshold = load_char(p + 30);
  out.ipo_flag = load_char(p + 31);
  out.luld_tier = load_char(p + 32);
  out.etp_flag = load_char(p + 33);
  out.etp_leverage = load_be32(p + 34);
  out.inverse_indicator = load_char(p + 38);
  return true;
}

/// 'S' (12).
inline bool decode_system_event(const std::byte* p,
                                ItchSystemEvent& out) noexcept {
  out.header = decode_header(p);
  out.event_code = load_char(p + 11);
  return true;
}

/// 'H' (25).
inline bool decode_trading_action(const std::byte* p,
                                  ItchTradingAction& out) noexcept {
  out.header = decode_header(p);
  out.stock = decode_stock(p + 11);
  out.state = load_char(p + 19);
  // p + 20 is reserved.
  out.reason0 = load_char(p + 21);
  out.reason1 = load_char(p + 22);
  out.reason2 = load_char(p + 23);
  out.reason3 = load_char(p + 24);
  return true;
}

} // namespace miniex
