/// \file random.hpp
/// \brief The one pseudo-random generator the project uses, and why.
///
/// Reproducibility is the requirement, not statistical purity: a `--seed`
/// must produce byte-identical flow on every platform and every compiler.
/// `std::mt19937` would do that too, but the *distributions* would not --
/// `std::uniform_int_distribution` is explicitly allowed to differ between
/// standard libraries, so the same seed gives different numbers under libc++
/// and libstdc++. That would break the differential tests in a way that
/// looks like an engine bug.
///
/// So: splitmix64 for seeding, xoshiro256** for the stream, and integer
/// range reduction written out by hand. All three are fixed here and cannot
/// drift.
#pragma once

#include <cstdint>

namespace miniex {

/// splitmix64 -- used only to expand a seed into the generator's state.
class SplitMix64 {
public:
  explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  constexpr std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

private:
  std::uint64_t state_;
};

/// xoshiro256** -- fast, small state, good enough for flow generation.
class Rng {
public:
  explicit Rng(std::uint64_t seed = 1) noexcept { reseed(seed); }

  void reseed(std::uint64_t seed) noexcept {
    SplitMix64 mix(seed);
    for (std::uint64_t& word : s_) word = mix.next();
  }

  std::uint64_t next() noexcept {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  /// Uniform in [0, bound). Lemire's multiply-shift with the rejection step
  /// written out, so the result does not depend on a library's choice of
  /// rejection strategy.
  std::uint32_t below(std::uint32_t bound) noexcept {
    if (bound == 0) return 0;
    std::uint64_t product =
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(next())) * bound;
    auto low = static_cast<std::uint32_t>(product);
    if (low < bound) {
      const std::uint32_t threshold = (~bound + 1U) % bound;
      while (low < threshold) {
        product =
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(next())) *
            bound;
        low = static_cast<std::uint32_t>(product);
      }
    }
    return static_cast<std::uint32_t>(product >> 32);
  }

  /// Uniform in [lo, hi] inclusive.
  std::uint32_t between(std::uint32_t lo, std::uint32_t hi) noexcept {
    return lo + below(hi - lo + 1);
  }

  /// True with probability percent/100.
  bool chance(std::uint32_t percent) noexcept { return below(100) < percent; }

  /// Geometric-ish draw: the number of successive `continue_percent` coin
  /// flips that came up heads, capped at `cap`.
  ///
  /// This is how depth away from the touch is placed. A uniform draw would
  /// put as much size 60 ticks out as at the touch, which is not what a book
  /// looks like; a geometric tail concentrates size near the touch and thins
  /// out behind it, which is.
  std::uint32_t geometric(std::uint32_t continue_percent,
                          std::uint32_t cap) noexcept {
    std::uint32_t n = 0;
    while (n < cap && chance(continue_percent)) ++n;
    return n;
  }

private:
  static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept {
    return (x << k) | (x >> (64 - k));
  }
  std::uint64_t s_[4]{};
};

} // namespace miniex
