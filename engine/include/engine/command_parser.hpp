/// \file command_parser.hpp
/// \brief Reader and writer for the plain-text command file format.
///
/// Tool and test code only: it allocates and it uses streams. The engine
/// never sees this file. The format is shared verbatim with the Python
/// oracle, so a reproducer minimised by the shrinker in `pyref/` can be fed
/// straight back into `engine_replay`.
///
///     # comments and blank lines are ignored
///     NEW     ts=<i64> id=<u64> side=<B|S> price=<i64> qty=<u32> \
///             type=<Limit|Market> tif=<Day|IOC|FOK|PostOnly> \
///             participant=<u16> symbol=<u16>
///     CANCEL  ts=<i64> id=<u64>
///     REPLACE ts=<i64> id=<u64> price=<i64> qty=<u32>
///
/// Every field is `key=value`, so field order does not matter and a missing
/// field is a parse error rather than a silent default -- except `ts`,
/// `participant` and `symbol`, which default to 0 because hand-written test
/// files should not have to carry them.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.hpp"

#include "engine/messages.hpp"

namespace miniex {

struct ParseError {
  std::size_t line{0};
  std::string message;
  [[nodiscard]] bool ok() const { return message.empty(); }
};

namespace detail {

inline bool split_kv(std::string_view token, std::string_view& key,
                     std::string_view& value) {
  const std::size_t eq = token.find('=');
  if (eq == std::string_view::npos) return false;
  key = token.substr(0, eq);
  value = token.substr(eq + 1);
  return true;
}

inline bool parse_side(std::string_view v, Side& out) {
  if (v == "B" || v == "Buy") {
    out = Side::Buy;
    return true;
  }
  if (v == "S" || v == "Sell") {
    out = Side::Sell;
    return true;
  }
  return false;
}

inline bool parse_type(std::string_view v, OrderType& out) {
  if (v == "Limit") {
    out = OrderType::Limit;
    return true;
  }
  if (v == "Market") {
    out = OrderType::Market;
    return true;
  }
  return false;
}

inline bool parse_tif(std::string_view v, TimeInForce& out) {
  if (v == "Day")
    out = TimeInForce::Day;
  else if (v == "IOC")
    out = TimeInForce::IOC;
  else if (v == "FOK")
    out = TimeInForce::FOK;
  else if (v == "PostOnly")
    out = TimeInForce::PostOnly;
  else
    return false;
  return true;
}

inline bool parse_i64(std::string_view v, std::int64_t& out) {
  if (v.empty()) return false;
  errno = 0;
  const std::string s(v);
  char* end = nullptr;
  const long long parsed = std::strtoll(s.c_str(), &end, 10);
  if (end == s.c_str() || *end != '\0' || errno != 0) return false;
  out = parsed;
  return true;
}

} // namespace detail

/// Parses one line. Returns false and fills \p error on a malformed line.
/// A blank or comment line returns true with \p produced set to false.
inline bool parse_command_line(std::string_view line, Command& out,
                               bool& produced, std::string& error) {
  produced = false;
  error.clear();

  // Strip a trailing comment and surrounding whitespace.
  const std::size_t hash = line.find('#');
  if (hash != std::string_view::npos) line = line.substr(0, hash);
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
    line.remove_prefix(1);
  while (!line.empty() &&
         (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
    line.remove_suffix(1);
  if (line.empty()) return true;

  std::vector<std::string_view> tokens;
  std::size_t pos = 0;
  while (pos < line.size()) {
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    const std::size_t start = pos;
    while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') ++pos;
    if (pos > start) tokens.push_back(line.substr(start, pos - start));
  }
  if (tokens.empty()) return true;

  const std::string_view verb = tokens[0];
  std::int64_t ts = 0;
  std::int64_t id = -1;
  std::int64_t price = 0;
  std::int64_t qty = -1;
  std::int64_t participant = 0;
  std::int64_t symbol = 0;
  Side side = Side::Buy;
  bool have_side = false;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::Day;

  for (std::size_t i = 1; i < tokens.size(); ++i) {
    std::string_view key;
    std::string_view value;
    if (!detail::split_kv(tokens[i], key, value)) {
      error = "expected key=value, got '" + std::string(tokens[i]) + "'";
      return false;
    }
    bool ok = true;
    if (key == "ts")
      ok = detail::parse_i64(value, ts);
    else if (key == "id")
      ok = detail::parse_i64(value, id);
    else if (key == "price")
      ok = detail::parse_i64(value, price);
    else if (key == "qty")
      ok = detail::parse_i64(value, qty);
    else if (key == "participant")
      ok = detail::parse_i64(value, participant);
    else if (key == "symbol")
      ok = detail::parse_i64(value, symbol);
    else if (key == "side")
      ok = have_side = detail::parse_side(value, side);
    else if (key == "type")
      ok = detail::parse_type(value, type);
    else if (key == "tif")
      ok = detail::parse_tif(value, tif);
    else {
      error = "unknown field '" + std::string(key) + "'";
      return false;
    }
    if (!ok) {
      error = "bad value for '" + std::string(key) + "': '" +
              std::string(value) + "'";
      return false;
    }
  }

  if (id < 0) {
    error = "missing id";
    return false;
  }

  if (verb == "NEW") {
    if (qty < 0) {
      error = "NEW requires qty";
      return false;
    }
    if (!have_side) {
      error = "NEW requires side";
      return false;
    }
    NewOrder n{};
    n.id = static_cast<OrderId>(id);
    n.price = price;
    n.qty = static_cast<Qty>(qty);
    n.symbol = static_cast<SymbolId>(symbol);
    n.participant = static_cast<ParticipantId>(participant);
    n.side = side;
    n.type = type;
    n.tif = tif;
    out = Command::make_new(ts, n);
  } else if (verb == "CANCEL") {
    out = Command::make_cancel(ts, static_cast<OrderId>(id));
  } else if (verb == "REPLACE") {
    if (qty < 0) {
      error = "REPLACE requires qty";
      return false;
    }
    out = Command::make_replace(ts, static_cast<OrderId>(id), price,
                                static_cast<Qty>(qty));
  } else {
    error = "unknown command '" + std::string(verb) + "'";
    return false;
  }
  produced = true;
  return true;
}

/// Parses a whole file's worth of text. On error the returned ParseError has
/// a non-empty message and the 1-based line number.
inline ParseError parse_commands(std::string_view text,
                                 std::vector<Command>& out) {
  std::size_t line_no = 0;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t nl = text.find('\n', pos);
    const std::string_view line = text.substr(
        pos, nl == std::string_view::npos ? text.size() - pos : nl - pos);
    ++line_no;
    Command c{};
    bool produced = false;
    std::string error;
    if (!parse_command_line(line, c, produced, error))
      return ParseError{line_no, error};
    if (produced) out.push_back(c);
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  return ParseError{};
}

/// The inverse, so a minimised reproducer can be written back out.
inline std::string to_line(const Command& c) {
  std::ostringstream os;
  switch (c.type) {
  case CommandType::New: {
    const NewOrder& n = c.body.neworder;
    os << "NEW ts=" << c.ts << " id=" << n.id << " side=" << side_char(n.side)
       << " price=" << n.price << " qty=" << n.qty
       << " type=" << to_string(n.type) << " tif=" << to_string(n.tif)
       << " participant=" << n.participant << " symbol=" << n.symbol;
    break;
  }
  case CommandType::Cancel:
    os << "CANCEL ts=" << c.ts << " id=" << c.body.cancel.id;
    break;
  case CommandType::Replace:
    os << "REPLACE ts=" << c.ts << " id=" << c.body.replace.id
       << " price=" << c.body.replace.new_price
       << " qty=" << c.body.replace.new_qty;
    break;
  }
  return os.str();
}

} // namespace miniex
