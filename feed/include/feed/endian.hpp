/// \file endian.hpp
/// \brief Big-endian loads from a possibly-unaligned byte stream.
///
/// Every multi-byte integer in ITCH is big-endian, and nothing in the file is
/// aligned to anything — the messages are packed back to back with a 2-byte
/// length prefix, so a 4-byte price routinely starts at an odd offset.
///
/// The idiom below is `memcpy` into a local followed by a byte swap. It looks
/// like a function call and a copy; it is neither. Every mainstream compiler
/// recognises a fixed-size small `memcpy` and emits a single unaligned load,
/// and the byte swap is one `bswap`/`rev` instruction. It is worth spelling
/// out because the obvious alternative —
/// `*reinterpret_cast<const std::uint32_t*>(p)` — is undefined behaviour on
/// two counts (strict aliasing and unaligned access) and UBSan will say so.
///
/// `std::byteswap` would be the C++23 spelling. The project is pinned to
/// C++20 (spec 0.1.14), so the builtins are used with a portable fallback.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace miniex {

#if defined(__GNUC__) || defined(__clang__)
#define MINIEX_BSWAP16(x) __builtin_bswap16(x)
#define MINIEX_BSWAP32(x) __builtin_bswap32(x)
#define MINIEX_BSWAP64(x) __builtin_bswap64(x)
#else
constexpr std::uint16_t miniex_bswap16(std::uint16_t v) {
  return static_cast<std::uint16_t>((v >> 8) | (v << 8));
}
constexpr std::uint32_t miniex_bswap32(std::uint32_t v) {
  return ((v >> 24) & 0x000000FFU) | ((v >> 8) & 0x0000FF00U) |
         ((v << 8) & 0x00FF0000U) | ((v << 24) & 0xFF000000U);
}
constexpr std::uint64_t miniex_bswap64(std::uint64_t v) {
  return (static_cast<std::uint64_t>(
              miniex_bswap32(static_cast<std::uint32_t>(v)))
          << 32) |
         miniex_bswap32(static_cast<std::uint32_t>(v >> 32));
}
#define MINIEX_BSWAP16(x) miniex_bswap16(x)
#define MINIEX_BSWAP32(x) miniex_bswap32(x)
#define MINIEX_BSWAP64(x) miniex_bswap64(x)
#endif

inline std::uint8_t load_u8(const std::byte* p) noexcept {
  return static_cast<std::uint8_t>(*p);
}

inline char load_char(const std::byte* p) noexcept {
  return static_cast<char>(*p);
}

inline std::uint16_t load_be16(const std::byte* p) noexcept {
  std::uint16_t v;
  std::memcpy(&v, p, 2);
  return MINIEX_BSWAP16(v);
}

inline std::uint32_t load_be32(const std::byte* p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, 4);
  return MINIEX_BSWAP32(v);
}

inline std::uint64_t load_be64(const std::byte* p) noexcept {
  std::uint64_t v;
  std::memcpy(&v, p, 8);
  return MINIEX_BSWAP64(v);
}

/// ITCH timestamps are 48-bit nanoseconds since midnight US/Eastern. They are
/// widened to 64 bits here and never carried around as a 6-byte type: a
/// 6-byte integer has no natural machine representation and every operation
/// on one would be another shift-and-mask.
///
/// The six bytes are copied into the *low* six bytes of a zeroed 64-bit
/// value's big-endian layout — i.e. at offset 2 — so that a single 64-bit
/// byte swap produces the right answer.
inline std::uint64_t load_be48(const std::byte* p) noexcept {
  std::uint64_t v = 0;
  std::memcpy(reinterpret_cast<std::byte*>(&v) + 2, p, 6);
  return MINIEX_BSWAP64(v);
}

} // namespace miniex
