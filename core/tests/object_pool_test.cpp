#include <cstdint>
#include <random>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "core/object_pool.hpp"
#include "core/types.hpp"

namespace miniex {
namespace {

struct Node {
  std::uint32_t next;
  std::uint32_t payload;
};
static_assert(PoolLinkable<Node>);
static_assert(PoolLinkable<Order>);

TEST(ObjectPool, HandsOutEveryIndexExactlyOnce) {
  ObjectPool<Node> pool(64);
  std::unordered_set<std::uint32_t> seen;
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t idx = pool.allocate();
    ASSERT_NE(idx, kInvalidIndex);
    EXPECT_LT(idx, 64U);
    EXPECT_TRUE(seen.insert(idx).second)
        << "index " << idx << " handed out twice";
  }
  EXPECT_EQ(pool.live_count(), 64U);
  EXPECT_EQ(pool.free_count(), 0U);
  EXPECT_TRUE(pool.exhausted());
}

TEST(ObjectPool, ExhaustionIsANormalConditionNotACrash) {
  ObjectPool<Node> pool(4);
  for (int i = 0; i < 4; ++i) ASSERT_NE(pool.allocate(), kInvalidIndex);
  EXPECT_EQ(pool.allocate(), kInvalidIndex);
  EXPECT_EQ(pool.allocate(), kInvalidIndex);
  EXPECT_EQ(pool.live_count(), 4U);
  EXPECT_TRUE(pool.verify());
}

TEST(ObjectPool, IndexZeroIsAValidHandle) {
  ObjectPool<Node> pool(4);
  bool saw_zero = false;
  for (int i = 0; i < 4; ++i) saw_zero |= (pool.allocate() == 0U);
  EXPECT_TRUE(saw_zero) << "index 0 must be usable; kInvalidIndex is all-ones";
}

TEST(ObjectPool, FreedIndexIsReused) {
  ObjectPool<Node> pool(2);
  const std::uint32_t a = pool.allocate();
  const std::uint32_t b = pool.allocate();
  ASSERT_EQ(pool.allocate(), kInvalidIndex);
  pool.deallocate(a);
  const std::uint32_t c = pool.allocate();
  EXPECT_EQ(c, a);
  EXPECT_NE(c, b);
}

TEST(ObjectPool, PayloadSurvivesUntilDeallocation) {
  ObjectPool<Node> pool(16);
  std::vector<std::uint32_t> handles;
  for (std::uint32_t i = 0; i < 16; ++i) {
    const std::uint32_t h = pool.allocate();
    pool[h].payload = i * 7 + 1;
    handles.push_back(h);
  }
  for (std::uint32_t i = 0; i < 16; ++i) {
    EXPECT_EQ(pool[handles[i]].payload, i * 7 + 1);
  }
}

TEST(ObjectPool, AccountingSurvivesRandomChurn) {
  ObjectPool<Node> pool(256);
  std::mt19937 rng(20260913);
  std::vector<std::uint32_t> live;
  for (int step = 0; step < 200'000; ++step) {
    const bool want_alloc = live.empty() || (rng() % 100) < 55;
    if (want_alloc && !pool.exhausted()) {
      const std::uint32_t h = pool.allocate();
      ASSERT_NE(h, kInvalidIndex);
      live.push_back(h);
    } else if (!live.empty()) {
      const std::size_t k = rng() % live.size();
      pool.deallocate(live[k]);
      live[k] = live.back();
      live.pop_back();
    }
    ASSERT_EQ(pool.live_count(), live.size());
  }
  EXPECT_TRUE(pool.verify());
  EXPECT_EQ(pool.live_count() + pool.free_count(), pool.capacity());
}

TEST(ObjectPool, ResetRestoresAFullFreeList) {
  ObjectPool<Node> pool(32);
  for (int i = 0; i < 20; ++i) (void)pool.allocate();
  EXPECT_EQ(pool.live_count(), 20U);
  pool.reset();
  EXPECT_EQ(pool.live_count(), 0U);
  EXPECT_EQ(pool.free_count(), 32U);
  EXPECT_TRUE(pool.verify());
}

TEST(ObjectPool, OrderRecordsFitAndStayCacheLineAligned) {
  ObjectPool<Order> pool(8);
  const std::uint32_t h = pool.allocate();
  ASSERT_NE(h, kInvalidIndex);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&pool[h]) % 64U, 0U)
      << "orders must not straddle cache lines";
}

} // namespace
} // namespace miniex
