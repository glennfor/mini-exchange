/// \file bitset_ladder.hpp
/// \brief Hierarchical bitset over the price ladder: O(1) best-price search.
///
/// When the touch is consumed, the book must find the next occupied price
/// level. A linear scan of the ladder array is O(window width) and the window
/// is 131072 ticks wide, so on a wide-spread or thin book that scan is the
/// single worst latency event in the engine -- exactly the case where
/// latency matters most.
///
/// The fix is a three-level bitset. Level 0 has one bit per tick. Level 1 has
/// one bit per level-0 *word*, set iff that word is non-zero. Level 2 has one
/// bit per level-1 word. At 2^17 ticks that is 2048 + 32 + 1 words: 16 KB,
/// which fits in L1/L2 comfortably. "Next set bit at or after i" is then at
/// most three `countr_zero`s and three loads, independent of how far away the
/// answer is.
///
/// The top level must be a single word, so the maximum supported width is
/// 64^3 = 262144 ticks; asserted below.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace miniex {

/// \tparam NBits number of ticks in the ladder window.
template <std::size_t NBits>
class BitsetLadder {
  static_assert(NBits > 0);
  static_assert(NBits <= 64ULL * 64 * 64,
                "three levels address at most 64^3 bits");

  static constexpr std::size_t kWordBits = 64;
  static constexpr std::size_t words_for(std::size_t bits) {
    return (bits + kWordBits - 1) / kWordBits;
  }

  static constexpr std::size_t kL0Words = words_for(NBits);
  static constexpr std::size_t kL1Words = words_for(kL0Words);
  static constexpr std::size_t kL2Words = words_for(kL1Words);
  static_assert(kL2Words == 1, "top summary level must be one word");

public:
  static constexpr std::size_t kSize = NBits;
  /// Returned when no set bit exists in the requested direction. Named for
  /// the standard-library convention rather than the project's `k` prefix.
  // NOLINTNEXTLINE(readability-identifier-naming)
  static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

  BitsetLadder() noexcept = default;

  void clear() noexcept {
    std::memset(l0_, 0, sizeof(l0_));
    std::memset(l1_, 0, sizeof(l1_));
    std::memset(l2_, 0, sizeof(l2_));
    count_ = 0;
  }

  [[nodiscard]] bool test(std::size_t i) const noexcept {
    return (l0_[i >> 6] >> (i & 63)) & 1ULL;
  }

  /// Idempotent. Sets the bit and every summary bit above it.
  void set(std::size_t i) noexcept {
    const std::size_t w0 = i >> 6;
    const std::uint64_t bit = 1ULL << (i & 63);
    if (l0_[w0] & bit) return;
    l0_[w0] |= bit;
    l1_[w0 >> 6] |= 1ULL << (w0 & 63);
    l2_[(w0 >> 6) >> 6] |= 1ULL << ((w0 >> 6) & 63);
    ++count_;
  }

  /// Idempotent. Clears summary bits only when the word below empties, which
  /// is what keeps this O(1) instead of a re-scan.
  void clear(std::size_t i) noexcept {
    const std::size_t w0 = i >> 6;
    const std::uint64_t bit = 1ULL << (i & 63);
    if (!(l0_[w0] & bit)) return;
    l0_[w0] &= ~bit;
    --count_;
    if (l0_[w0] != 0) return;

    const std::size_t w1 = w0 >> 6;
    l1_[w1] &= ~(1ULL << (w0 & 63));
    if (l1_[w1] != 0) return;

    l2_[w1 >> 6] &= ~(1ULL << (w1 & 63));
  }

  /// Lowest set bit index >= \p i, or npos.
  [[nodiscard]] std::size_t
  find_next_set_at_or_after(std::size_t i) const noexcept {
    if (i >= NBits) return npos;

    // Level 0: the rest of i's own word.
    std::size_t w0 = i >> 6;
    std::uint64_t m = l0_[w0] & (~0ULL << (i & 63));
    if (m != 0)
      return (w0 << 6) | static_cast<std::size_t>(std::countr_zero(m));

    // Level 1: the next non-empty level-0 word.
    const std::size_t b1 = w0 + 1;
    if (b1 < kL0Words) {
      std::size_t w1 = b1 >> 6;
      m = l1_[w1] & (~0ULL << (b1 & 63));
      if (m != 0) {
        w0 = (w1 << 6) | static_cast<std::size_t>(std::countr_zero(m));
        return (w0 << 6) | static_cast<std::size_t>(std::countr_zero(l0_[w0]));
      }
      // Level 2: the next non-empty level-1 word.
      const std::size_t b2 = w1 + 1;
      if (b2 < kL1Words) {
        m = l2_[0] & (~0ULL << b2);
        if (m != 0) {
          w1 = static_cast<std::size_t>(std::countr_zero(m));
          w0 = (w1 << 6) | static_cast<std::size_t>(std::countr_zero(l1_[w1]));
          return (w0 << 6) |
                 static_cast<std::size_t>(std::countr_zero(l0_[w0]));
        }
      }
    }
    return npos;
  }

  /// Highest set bit index <= \p i, or npos.
  [[nodiscard]] std::size_t
  find_prev_set_at_or_before(std::size_t i) const noexcept {
    if (NBits == 0) return npos;
    if (i >= NBits) i = NBits - 1;

    std::size_t w0 = i >> 6;
    const unsigned top = static_cast<unsigned>(i & 63);
    std::uint64_t m = l0_[w0] & mask_up_to(top);
    if (m != 0) return (w0 << 6) | highest_bit(m);

    if (w0 == 0) return npos;
    const std::size_t b1 = w0 - 1;
    std::size_t w1 = b1 >> 6;
    m = l1_[w1] & mask_up_to(static_cast<unsigned>(b1 & 63));
    if (m != 0) {
      w0 = (w1 << 6) | highest_bit(m);
      return (w0 << 6) | highest_bit(l0_[w0]);
    }
    if (w1 == 0) return npos;
    const std::size_t b2 = w1 - 1;
    m = l2_[0] & mask_up_to(static_cast<unsigned>(b2 & 63));
    if (m != 0) {
      w1 = highest_bit(m);
      w0 = (w1 << 6) | highest_bit(l1_[w1]);
      return (w0 << 6) | highest_bit(l0_[w0]);
    }
    return npos;
  }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] bool any() const noexcept { return count_ != 0; }
  [[nodiscard]] static constexpr std::size_t size() noexcept { return NBits; }

  /// Bytes of summary metadata, for the sizing discussion in the docs.
  [[nodiscard]] static constexpr std::size_t footprint_bytes() noexcept {
    return (kL0Words + kL1Words + kL2Words) * sizeof(std::uint64_t);
  }

private:
  /// Bits 0..b inclusive.
  static constexpr std::uint64_t mask_up_to(unsigned b) noexcept {
    return b == 63 ? ~0ULL : ((1ULL << (b + 1)) - 1);
  }
  static std::size_t highest_bit(std::uint64_t w) noexcept {
    return 63 - static_cast<std::size_t>(std::countl_zero(w));
  }

  std::uint64_t l0_[kL0Words]{};
  std::uint64_t l1_[kL1Words]{};
  std::uint64_t l2_[kL2Words]{};
  std::size_t count_{0};
};

} // namespace miniex
