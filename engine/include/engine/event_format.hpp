/// \file event_format.hpp
/// \brief The canonical text form of an outbound event.
///
/// This format is the comparison surface between three independent
/// implementations: the C++ engine over `NaiveBook`, the same engine over
/// `FastBook`, and the Python reference oracle in `pyref/`. Agreement is
/// checked by `diff`, so the format has exactly two requirements: every field
/// that could differ must appear, and it must be trivial to reproduce in
/// another language.
///
/// Hence: one line per event, the same nine fields in the same order for
/// every event type, no optional fields, no alignment, no locale-dependent
/// formatting. It is verbose to read and impossible to get subtly wrong,
/// which is the right trade for a differential-testing artefact.
///
///     <seq> <TYPE> id=<u64> counter=<u64> match=<u64> side=<B|S>
///           price=<i64> qty=<u32> rem=<u32> reason=<Name>
///
/// Writing goes through a caller-provided buffer so the tools can emit the
/// stream without allocating per event.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/types.hpp"

#include "engine/messages.hpp"

namespace miniex {

namespace detail {

/// Appends a decimal integer. Returns the new offset. No locale, no iostreams.
inline std::size_t append_u64(char* buf, std::size_t n, std::uint64_t v) {
  char tmp[20];
  std::size_t k = 0;
  do {
    tmp[k++] = static_cast<char>('0' + (v % 10));
    v /= 10;
  } while (v != 0);
  while (k > 0) buf[n++] = tmp[--k];
  return n;
}

inline std::size_t append_i64(char* buf, std::size_t n, std::int64_t v) {
  if (v < 0) {
    buf[n++] = '-';
    // Negated as unsigned so INT64_MIN does not overflow.
    return append_u64(buf, n, ~static_cast<std::uint64_t>(v) + 1U);
  }
  return append_u64(buf, n, static_cast<std::uint64_t>(v));
}

inline std::size_t append_str(char* buf, std::size_t n, const char* s) {
  while (*s != '\0') buf[n++] = *s++;
  return n;
}

} // namespace detail

/// Longest possible line, with room to spare.
///
/// Worst case, field by field: seq(20) + ' ' + "CANCELLED"(9) + " id="(4)+20
/// + " counter="(9)+20 + " match="(7)+20 + " side="(6)+1 + " price="(7)+20
/// + " qty="(5)+10 + " rem="(5)+10 + " reason="(8)+22 = 204 bytes. The
/// constant is 256 so that adding a field does not silently overrun; the
/// `EveryEventTypeFitsInTheDeclaredBuffer` test fills every field with its
/// maximum value and fails if the margin is gone.
inline constexpr std::size_t kEventLineMax = 256;

/// Writes one event line (no trailing newline) into \p buf. Returns length.
inline std::size_t format_event(const Event& e, char* buf,
                                std::size_t cap) noexcept {
  assert(cap >= kEventLineMax &&
         "format_event writes up to kEventLineMax bytes");
  (void)cap;
  std::size_t n = 0;
  n = detail::append_u64(buf, n, e.seq);
  buf[n++] = ' ';
  n = detail::append_str(buf, n, to_string(e.type));
  n = detail::append_str(buf, n, " id=");
  n = detail::append_u64(buf, n, e.id);
  n = detail::append_str(buf, n, " counter=");
  n = detail::append_u64(buf, n, e.counter_id);
  n = detail::append_str(buf, n, " match=");
  n = detail::append_u64(buf, n, e.match_id);
  n = detail::append_str(buf, n, " side=");
  buf[n++] = side_char(e.side);
  n = detail::append_str(buf, n, " price=");
  n = detail::append_i64(buf, n, e.price);
  n = detail::append_str(buf, n, " qty=");
  n = detail::append_u64(buf, n, e.qty);
  n = detail::append_str(buf, n, " rem=");
  n = detail::append_u64(buf, n, e.qty_remaining);
  n = detail::append_str(buf, n, " reason=");
  n = detail::append_str(buf, n, to_string(e.reason));
  return n;
}

/// Convenience for tests and tools; allocates, so not for the hot path.
inline std::string to_line(const Event& e) {
  char buf[kEventLineMax];
  const std::size_t n = format_event(e, buf, sizeof(buf));
  return std::string(buf, n);
}

} // namespace miniex
