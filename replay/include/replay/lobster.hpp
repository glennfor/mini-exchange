/// \file lobster.hpp
/// \brief LOBSTER message and orderbook CSV loading.
///
/// LOBSTER publishes two files per symbol-day, in lockstep: a *message* file
/// and an *orderbook* file with one row each. Row *i* of the orderbook file is
/// the state of the book **after** message *i*. That lockstep is what makes
/// event-by-event validation possible, and it is the whole reason this project
/// uses LOBSTER rather than eyeballing a depth feed.
///
/// ## Message file columns
///
/// `time, type, order_id, size, price, direction`
///
/// * `time` — seconds after midnight, with decimals. Parsed as **two integer
///   fields split on the decimal point**, never through a float: a `double`
///   holds about 15 significant digits and `34200.123456789` needs 14, so
///   parsing through one is close enough to work and close enough to be wrong
///   at the last digit, which is exactly the kind of bug that costs a day.
/// * `type` — 1 submission, 2 partial cancellation, 3 deletion, 4 execution of
///   a visible limit order, 5 execution of a hidden order, 6 auction/cross,
///   7 trading halt.
/// * `price` — dollars times 10000, i.e. this project's `Price` scale exactly.
/// * `direction` — 1 for buy, -1 for sell.
///
/// ## The direction trap
///
/// > For **execution** messages (types 4 and 5) the direction field describes
/// > the side of the **limit order that was executed**, not the aggressor.
///
/// A sell market order lifting a bid appears with direction `1`. Reading it as
/// the aggressor's side inverts every inferred trade sign in the file, and the
/// book reconstruction still comes out right — so the bug hides until someone
/// computes order-flow imbalance and gets the sign backwards. It is the single
/// most common LOBSTER mistake and it has its own test.
#pragma once

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.hpp"

namespace miniex {

enum class LobsterType : std::uint8_t {
  Submission = 1,
  PartialCancel = 2,
  Deletion = 3,
  VisibleExecution = 4,
  HiddenExecution = 5,
  AuctionCross = 6,
  TradingHalt = 7,
};

constexpr const char* to_string(LobsterType t) noexcept {
  switch (t) {
  case LobsterType::Submission: return "Submission";
  case LobsterType::PartialCancel: return "PartialCancel";
  case LobsterType::Deletion: return "Deletion";
  case LobsterType::VisibleExecution: return "VisibleExecution";
  case LobsterType::HiddenExecution: return "HiddenExecution";
  case LobsterType::AuctionCross: return "AuctionCross";
  case LobsterType::TradingHalt: return "TradingHalt";
  }
  return "Unknown";
}

struct LobsterMessage {
  TsNanos ts{0}; ///< nanoseconds after midnight
  LobsterType type{LobsterType::Submission};
  OrderId order_id{0};
  Qty size{0};
  Price price{0};
  /// The side of the order this message concerns. For an execution that is
  /// the side of the **resting** order, not the aggressor -- see the file
  /// comment.
  Side side{Side::Buy};

  /// The aggressor's side, which is the opposite of the resting order's for
  /// an execution. Provided as a named function precisely so that the
  /// inversion is written down once instead of being re-derived (and
  /// re-mistaken) at every call site.
  [[nodiscard]] Side aggressor_side() const noexcept {
    return type == LobsterType::VisibleExecution ||
                   type == LobsterType::HiddenExecution
               ? opposite(side)
               : side;
  }

  /// True when this message changes the visible book. Hidden executions,
  /// auction crosses and halts do not.
  [[nodiscard]] bool changes_visible_book() const noexcept {
    return type == LobsterType::Submission ||
           type == LobsterType::PartialCancel ||
           type == LobsterType::Deletion ||
           type == LobsterType::VisibleExecution;
  }
};

/// One row of the orderbook file: `ask_price_1, ask_size_1, bid_price_1,
/// bid_size_1, ...` for N levels.
struct LobsterBookRow {
  std::vector<Price> ask_price;
  std::vector<AggQty> ask_size;
  std::vector<Price> bid_price;
  std::vector<AggQty> bid_size;

  [[nodiscard]] std::size_t levels() const { return ask_price.size(); }
};

struct LobsterParseError {
  std::size_t line{0};
  std::string message;
  [[nodiscard]] bool ok() const { return message.empty(); }
};

namespace detail {

/// Splits `34200.017459617` into integer nanoseconds without touching a
/// float. The fractional part is padded or truncated to exactly nine digits.
inline bool parse_seconds_to_nanos(std::string_view text, TsNanos& out) {
  if (text.empty()) return false;
  const std::size_t dot = text.find('.');
  const std::string_view whole = text.substr(0, dot);
  std::int64_t seconds = 0;
  for (char c : whole) {
    if (c < '0' || c > '9') return false;
    seconds = seconds * 10 + (c - '0');
  }
  std::int64_t nanos = 0;
  if (dot != std::string_view::npos) {
    const std::string_view frac = text.substr(dot + 1);
    for (std::size_t i = 0; i < 9; ++i) {
      const char c = i < frac.size() ? frac[i] : '0';
      if (c < '0' || c > '9') return false;
      nanos = nanos * 10 + (c - '0');
    }
  }
  out = seconds * 1'000'000'000 + nanos;
  return true;
}

inline bool parse_i64(std::string_view text, std::int64_t& out) {
  if (text.empty()) return false;
  bool negative = false;
  std::size_t i = 0;
  if (text[0] == '-' || text[0] == '+') {
    negative = text[0] == '-';
    i = 1;
  }
  if (i >= text.size()) return false;
  std::int64_t value = 0;
  for (; i < text.size(); ++i) {
    // LOBSTER occasionally writes an integer with a trailing ".0".
    if (text[i] == '.') break;
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + (text[i] - '0');
  }
  out = negative ? -value : value;
  return true;
}

inline void split_csv(std::string_view line,
                      std::vector<std::string_view>& out) {
  out.clear();
  std::size_t start = 0;
  while (start <= line.size()) {
    const std::size_t comma = line.find(',', start);
    const std::size_t end =
        comma == std::string_view::npos ? line.size() : comma;
    std::string_view field = line.substr(start, end - start);
    while (!field.empty() && (field.front() == ' ' || field.front() == '\t'))
      field.remove_prefix(1);
    while (!field.empty() && (field.back() == ' ' || field.back() == '\t' ||
                              field.back() == '\r'))
      field.remove_suffix(1);
    out.push_back(field);
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
}

} // namespace detail

/// Parses one message line. Returns false with `error` set on a bad line.
inline bool parse_lobster_message(std::string_view line, LobsterMessage& out,
                                  std::string& error) {
  std::vector<std::string_view> fields;
  detail::split_csv(line, fields);
  if (fields.size() < 6) {
    error = "expected 6 columns, got " + std::to_string(fields.size());
    return false;
  }
  if (!detail::parse_seconds_to_nanos(fields[0], out.ts)) {
    error = "bad time '" + std::string(fields[0]) + "'";
    return false;
  }
  std::int64_t value = 0;
  if (!detail::parse_i64(fields[1], value) || value < 1 || value > 7) {
    error = "bad type '" + std::string(fields[1]) + "'";
    return false;
  }
  out.type = static_cast<LobsterType>(value);
  if (!detail::parse_i64(fields[2], value)) {
    error = "bad order id '" + std::string(fields[2]) + "'";
    return false;
  }
  out.order_id = static_cast<OrderId>(value);
  if (!detail::parse_i64(fields[3], value) || value < 0) {
    error = "bad size '" + std::string(fields[3]) + "'";
    return false;
  }
  out.size = static_cast<Qty>(value);
  if (!detail::parse_i64(fields[4], value)) {
    error = "bad price '" + std::string(fields[4]) + "'";
    return false;
  }
  out.price = value;
  if (!detail::parse_i64(fields[5], value) || (value != 1 && value != -1)) {
    error = "bad direction '" + std::string(fields[5]) + "'";
    return false;
  }
  out.side = value == 1 ? Side::Buy : Side::Sell;
  return true;
}

/// Parses one orderbook line: `ask_price_1, ask_size_1, bid_price_1,
/// bid_size_1, ...`.
inline bool parse_lobster_book(std::string_view line, LobsterBookRow& out,
                               std::string& error) {
  std::vector<std::string_view> fields;
  detail::split_csv(line, fields);
  if (fields.empty() || fields.size() % 4 != 0) {
    error = "expected a multiple of 4 columns, got " +
            std::to_string(fields.size());
    return false;
  }
  const std::size_t levels = fields.size() / 4;
  out.ask_price.assign(levels, kInvalidPrice);
  out.ask_size.assign(levels, 0);
  out.bid_price.assign(levels, kInvalidPrice);
  out.bid_size.assign(levels, 0);

  for (std::size_t i = 0; i < levels; ++i) {
    std::int64_t value = 0;
    const std::size_t base = i * 4;
    if (!detail::parse_i64(fields[base], value)) {
      error = "bad ask price at level " + std::to_string(i + 1);
      return false;
    }
    out.ask_price[i] = value;
    if (!detail::parse_i64(fields[base + 1], value)) {
      error = "bad ask size at level " + std::to_string(i + 1);
      return false;
    }
    out.ask_size[i] = static_cast<AggQty>(value < 0 ? 0 : value);
    if (!detail::parse_i64(fields[base + 2], value)) {
      error = "bad bid price at level " + std::to_string(i + 1);
      return false;
    }
    out.bid_price[i] = value;
    if (!detail::parse_i64(fields[base + 3], value)) {
      error = "bad bid size at level " + std::to_string(i + 1);
      return false;
    }
    out.bid_size[i] = static_cast<AggQty>(value < 0 ? 0 : value);
  }
  return true;
}

/// LOBSTER fills absent levels with a sentinel price (9999999999 on the ask
/// side, -9999999999 on the bid side) and size 0. Recognising the sentinel is
/// how "the book is thinner than 10 levels" is told apart from "there is a
/// level here at an absurd price".
inline bool is_lobster_empty_level(Price price, AggQty size) noexcept {
  return size == 0 || price >= 9999999999LL || price <= -9999999999LL;
}

/// Loads a whole message file.
inline LobsterParseError
load_lobster_messages(const std::string& path,
                      std::vector<LobsterMessage>& out) {
  std::ifstream in(path);
  if (!in) return LobsterParseError{0, "cannot open " + path};
  std::string line;
  std::size_t line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    if (line.empty()) continue;
    LobsterMessage msg{};
    std::string error;
    if (!parse_lobster_message(line, msg, error))
      return LobsterParseError{line_no, error};
    out.push_back(msg);
  }
  return LobsterParseError{};
}

} // namespace miniex
