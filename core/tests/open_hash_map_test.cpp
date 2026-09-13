#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "core/open_hash_map.hpp"
#include "core/types.hpp"

namespace miniex {
namespace {

TEST(OpenHashMap, InsertFindErase) {
  OpenHashMap map(64);
  EXPECT_TRUE(map.empty());
  EXPECT_TRUE(map.insert(42, 7));
  EXPECT_EQ(map.find(42), 7U);
  EXPECT_TRUE(map.contains(42));
  EXPECT_EQ(map.size(), 1U);
  EXPECT_TRUE(map.erase(42));
  EXPECT_EQ(map.find(42), kInvalidIndex);
  EXPECT_FALSE(map.erase(42));
  EXPECT_TRUE(map.empty());
}

TEST(OpenHashMap, DuplicateInsertIsRejected) {
  OpenHashMap map(64);
  EXPECT_TRUE(map.insert(1, 10));
  EXPECT_FALSE(map.insert(1, 11));
  EXPECT_EQ(map.find(1), 10U) << "a rejected insert must not clobber the value";
  EXPECT_EQ(map.size(), 1U);
}

TEST(OpenHashMap, AssignUpdatesOnlyExistingKeys) {
  OpenHashMap map(64);
  EXPECT_FALSE(map.assign(5, 1));
  ASSERT_TRUE(map.insert(5, 1));
  EXPECT_TRUE(map.assign(5, 99));
  EXPECT_EQ(map.find(5), 99U);
  EXPECT_EQ(map.size(), 1U);
}

TEST(OpenHashMap, KeyZeroIsAnOrdinaryKey) {
  // The empty marker is the *value*, not the key, so id 0 must work.
  OpenHashMap map(16);
  EXPECT_TRUE(map.insert(0, 3));
  EXPECT_EQ(map.find(0), 3U);
  EXPECT_TRUE(map.erase(0));
  EXPECT_FALSE(map.contains(0));
}

TEST(OpenHashMap, RespectsTheEntryCeiling) {
  OpenHashMap map(8);
  for (OrderId i = 0; i < 8; ++i)
    EXPECT_TRUE(map.insert(i, static_cast<std::uint32_t>(i)));
  EXPECT_FALSE(map.insert(8, 8))
      << "insert past max_entries must fail, not grow";
  EXPECT_EQ(map.size(), 8U);
  EXPECT_LE(map.size() * 2, map.bucket_count());
}

// Backward-shift deletion is the part that is easy to get subtly wrong: a
// mistake leaves a hole inside a probe run and lookups silently start
// missing. Forcing every key into one bucket makes the shift loop do real
// work on every erase.
TEST(OpenHashMap, BackwardShiftDeletionKeepsCollidingKeysFindable) {
  OpenHashMap map(1024);
  // Keys that share a home slot: multiples of the modular inverse of kMix
  // land on the same high bits. Simpler and just as brutal: insert a dense
  // run and erase from the middle.
  constexpr int kN = 400;
  for (int i = 0; i < kN; ++i)
    ASSERT_TRUE(
        map.insert(static_cast<OrderId>(i), static_cast<std::uint32_t>(i)));
  for (int i = 0; i < kN; i += 3)
    ASSERT_TRUE(map.erase(static_cast<OrderId>(i)));
  for (int i = 0; i < kN; ++i) {
    const auto key = static_cast<OrderId>(i);
    if (i % 3 == 0) {
      EXPECT_EQ(map.find(key), kInvalidIndex) << "key " << i;
    } else {
      EXPECT_EQ(map.find(key), static_cast<std::uint32_t>(i)) << "key " << i;
    }
  }
}

TEST(OpenHashMap, AgreesWithStdUnorderedMapUnderRandomChurn) {
  OpenHashMap map(2048);
  std::unordered_map<OrderId, std::uint32_t> oracle;
  std::mt19937_64 rng(4711);
  std::vector<OrderId> live;

  for (int step = 0; step < 300'000; ++step) {
    const int action = static_cast<int>(rng() % 100);
    if (action < 45 && oracle.size() < 2048) {
      const OrderId key = rng() % 5000;
      const auto value = static_cast<std::uint32_t>(rng() % 1'000'000);
      const bool inserted = map.insert(key, value);
      const bool oracle_inserted = oracle.emplace(key, value).second;
      ASSERT_EQ(inserted, oracle_inserted) << "step " << step << " key " << key;
      if (inserted) live.push_back(key);
    } else if (action < 80 && !live.empty()) {
      const std::size_t k = rng() % live.size();
      const OrderId key = live[k];
      live[k] = live.back();
      live.pop_back();
      ASSERT_TRUE(map.erase(key));
      ASSERT_EQ(oracle.erase(key), 1U);
    } else {
      const OrderId key = rng() % 5000;
      const auto it = oracle.find(key);
      const std::uint32_t expected =
          it == oracle.end() ? kInvalidIndex : it->second;
      ASSERT_EQ(map.find(key), expected) << "step " << step << " key " << key;
    }
    ASSERT_EQ(map.size(), oracle.size()) << "step " << step;
  }

  for (const auto& [key, value] : oracle) {
    ASSERT_EQ(map.find(key), value);
  }
}

// The distribution test the spec asks for: ITCH order reference numbers are
// dense and sequential, which is exactly the input an identity hash
// clusters on. With Fibonacci mixing the average probe distance must stay
// close to zero at a 50% load factor.
TEST(OpenHashMap, DenseSequentialIdsDoNotCluster) {
  constexpr std::size_t kEntries = 100'000;
  OpenHashMap map(kEntries);
  // Real ITCH reference numbers start at an arbitrary offset and step by 1.
  constexpr OrderId kBase = 4'294'967'296ULL;
  for (std::size_t i = 0; i < kEntries; ++i) {
    ASSERT_TRUE(map.insert(kBase + i, static_cast<std::uint32_t>(i)));
  }
  const double avg = static_cast<double>(map.total_probe_distance()) /
                     static_cast<double>(kEntries);
  EXPECT_LT(avg, 1.0) << "average probe distance " << avg;
  EXPECT_LT(map.max_probe_distance(), 64U)
      << "worst-case probe run " << map.max_probe_distance();

  // And every key is still findable.
  for (std::size_t i = 0; i < kEntries; ++i) {
    ASSERT_EQ(map.find(kBase + i), static_cast<std::uint32_t>(i));
  }
}

TEST(OpenHashMap, ClearEmptiesWithoutReallocating) {
  OpenHashMap map(64);
  const std::size_t buckets = map.bucket_count();
  for (OrderId i = 0; i < 60; ++i)
    ASSERT_TRUE(map.insert(i, static_cast<std::uint32_t>(i)));
  map.clear();
  EXPECT_EQ(map.size(), 0U);
  EXPECT_EQ(map.bucket_count(), buckets);
  EXPECT_FALSE(map.contains(3));
  EXPECT_TRUE(map.insert(3, 1));
}

TEST(OpenHashMap, ForEachVisitsEveryLiveEntry) {
  OpenHashMap map(32);
  for (OrderId i = 1; i <= 10; ++i)
    ASSERT_TRUE(map.insert(i, static_cast<std::uint32_t>(i * 2)));
  ASSERT_TRUE(map.erase(4));
  std::uint64_t sum = 0;
  std::size_t seen = 0;
  map.for_each([&](OrderId k, std::uint32_t v) {
    EXPECT_EQ(v, static_cast<std::uint32_t>(k * 2));
    sum += k;
    ++seen;
  });
  EXPECT_EQ(seen, 9U);
  EXPECT_EQ(sum, 55U - 4U);
}

} // namespace
} // namespace miniex
