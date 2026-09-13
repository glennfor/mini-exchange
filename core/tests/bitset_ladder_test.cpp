#include <cstddef>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "core/bitset_ladder.hpp"
#include "core/config.hpp"

namespace miniex {
namespace {

/// The deliberately obvious reference: a flat vector scanned linearly. Every
/// hierarchical result is compared against this.
class NaiveBitset {
public:
  explicit NaiveBitset(std::size_t n) : bits_(n, false) {}
  void set(std::size_t i) { bits_[i] = true; }
  void clear(std::size_t i) { bits_[i] = false; }
  [[nodiscard]] bool test(std::size_t i) const { return bits_[i]; }
  [[nodiscard]] std::size_t next(std::size_t i) const {
    for (std::size_t k = i; k < bits_.size(); ++k)
      if (bits_[k]) return k;
    return kNpos;
  }
  [[nodiscard]] std::size_t prev(std::size_t i) const {
    if (bits_.empty()) return kNpos;
    if (i >= bits_.size()) i = bits_.size() - 1;
    for (std::size_t k = i + 1; k-- > 0;)
      if (bits_[k]) return k;
    return kNpos;
  }
  [[nodiscard]] std::size_t count() const {
    std::size_t c = 0;
    for (bool b : bits_) c += b ? 1 : 0;
    return c;
  }
  static constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

private:
  std::vector<bool> bits_;
};

template <std::size_t N>
void expect_agrees(const BitsetLadder<N>& fast, const NaiveBitset& slow) {
  ASSERT_EQ(fast.count(), slow.count());
  for (std::size_t i = 0; i < N; ++i) {
    ASSERT_EQ(fast.test(i), slow.test(i)) << "bit " << i;
    ASSERT_EQ(fast.find_next_set_at_or_after(i), slow.next(i))
        << "next from " << i;
    ASSERT_EQ(fast.find_prev_set_at_or_before(i), slow.prev(i))
        << "prev from " << i;
  }
}

TEST(BitsetLadder, EmptyFindsNothing) {
  BitsetLadder<512> b;
  EXPECT_EQ(b.count(), 0U);
  EXPECT_FALSE(b.any());
  EXPECT_EQ(b.find_next_set_at_or_after(0), b.npos);
  EXPECT_EQ(b.find_prev_set_at_or_before(511), b.npos);
  EXPECT_EQ(b.find_next_set_at_or_after(1000), b.npos);
}

TEST(BitsetLadder, SingleBitAtEveryWordBoundaryIsFound) {
  constexpr std::size_t kN = 4096;
  const std::size_t interesting[] = {0,   1,   63,  64,  65,   127, 128,
                                     129, 511, 512, 513, 4094, 4095};
  for (std::size_t i : interesting) {
    BitsetLadder<kN> b;
    NaiveBitset n(kN);
    b.set(i);
    n.set(i);
    EXPECT_EQ(b.count(), 1U) << "bit " << i;
    EXPECT_TRUE(b.test(i));
    expect_agrees(b, n);
  }
}

TEST(BitsetLadder, FullLadderAgreesWithNaive) {
  constexpr std::size_t kN = 1024;
  BitsetLadder<kN> b;
  NaiveBitset n(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    b.set(i);
    n.set(i);
  }
  EXPECT_EQ(b.count(), kN);
  expect_agrees(b, n);
}

TEST(BitsetLadder, SetAndClearAreIdempotent) {
  BitsetLadder<256> b;
  b.set(100);
  b.set(100);
  EXPECT_EQ(b.count(), 1U);
  b.clear(100);
  b.clear(100);
  EXPECT_EQ(b.count(), 0U);
  EXPECT_EQ(b.find_next_set_at_or_after(0), b.npos);
}

TEST(BitsetLadder, ClearingOneOfTwoBitsInAWordKeepsSummariesLive) {
  BitsetLadder<4096> b;
  b.set(70);
  b.set(71);
  b.clear(70);
  EXPECT_EQ(b.find_next_set_at_or_after(0), 71U);
  b.clear(71);
  EXPECT_EQ(b.find_next_set_at_or_after(0), b.npos);
}

TEST(BitsetLadder, RandomPatternsAgreeWithNaive) {
  constexpr std::size_t kN = 8192;
  std::mt19937_64 rng(90210);
  for (int trial = 0; trial < 8; ++trial) {
    BitsetLadder<kN> b;
    NaiveBitset n(kN);
    const std::size_t density = 1U << (trial % 8);
    for (std::size_t i = 0; i < kN; ++i) {
      if (rng() % density == 0) {
        b.set(i);
        n.set(i);
      }
    }
    // Then churn it: clearing is where the summary bookkeeping goes wrong.
    for (int k = 0; k < 2000; ++k) {
      const std::size_t i = rng() % kN;
      if (rng() % 2) {
        b.set(i);
        n.set(i);
      } else {
        b.clear(i);
        n.clear(i);
      }
    }
    expect_agrees(b, n);
  }
}

// The real configured width, exercised sparsely -- an exhaustive scan of
// 131072 bits times 131072 queries is too slow, so probe the boundaries.
TEST(BitsetLadder, ConfiguredLadderWidthBehavesAtTheExtremes) {
  BitsetLadder<kLadderTicks> b;
  EXPECT_EQ(b.size(), kLadderTicks);
  b.set(0);
  b.set(kLadderTicks - 1);
  b.set(kLadderTicks / 2);
  EXPECT_EQ(b.find_next_set_at_or_after(0), 0U);
  EXPECT_EQ(b.find_next_set_at_or_after(1), kLadderTicks / 2);
  EXPECT_EQ(b.find_next_set_at_or_after(kLadderTicks / 2 + 1),
            kLadderTicks - 1);
  EXPECT_EQ(b.find_next_set_at_or_after(kLadderTicks), b.npos);
  EXPECT_EQ(b.find_prev_set_at_or_before(kLadderTicks - 1), kLadderTicks - 1);
  EXPECT_EQ(b.find_prev_set_at_or_before(kLadderTicks - 2), kLadderTicks / 2);
  EXPECT_EQ(b.find_prev_set_at_or_before(kLadderTicks / 2 - 1), 0U);
  b.clear(0);
  EXPECT_EQ(b.find_prev_set_at_or_before(kLadderTicks / 2 - 1), b.npos);
}

TEST(BitsetLadder, SummaryFootprintIsSmallEnoughToStayResident) {
  // 2^17 ticks -> 2048 + 32 + 1 words = 16648 bytes. If this grows past L2
  // the whole point of the structure is lost, so it is asserted, not assumed.
  EXPECT_EQ(BitsetLadder<kLadderTicks>::footprint_bytes(),
            (2048U + 32U + 1U) * 8U);
}

TEST(BitsetLadder, ClearResetsEverything) {
  BitsetLadder<1024> b;
  for (std::size_t i = 0; i < 1024; i += 7) b.set(i);
  EXPECT_GT(b.count(), 0U);
  b.clear();
  EXPECT_EQ(b.count(), 0U);
  EXPECT_EQ(b.find_next_set_at_or_after(0), b.npos);
}

} // namespace
} // namespace miniex
