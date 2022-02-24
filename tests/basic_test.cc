#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "bufpool/pool.h"

using bufpool::Config;
using bufpool::Pool;

TEST(SizeClass, RoundsUpToPowerOfTwo) {
  EXPECT_EQ(bufpool::SizeClassFor(1), 0);
  EXPECT_EQ(bufpool::SizeClassFor(256), 0);
  EXPECT_EQ(bufpool::SizeClassFor(257), 1);
  EXPECT_EQ(bufpool::SizeClassFor(4096), 4);
  EXPECT_EQ(bufpool::SizeClassFor(1 << 20), int(bufpool::kNumClasses) - 1);
  EXPECT_EQ(bufpool::SizeClassFor((1 << 20) + 1), -1);
  EXPECT_EQ(bufpool::ClassSize(0), 256u);
  EXPECT_EQ(bufpool::ClassSize(4), 4096u);
}

TEST(Alignment, SlotsAreNaturallyAligned) {
  Pool pool;
  for (size_t size : {1u, 100u, 256u, 300u, 4000u, 65536u, 700000u, 1u << 20}) {
    void* p = pool.Allocate(size);
    ASSERT_NE(p, nullptr);
    const size_t slot = bufpool::ClassSize(bufpool::SizeClassFor(size));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % slot, 0u) << "size " << size;
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % alignof(std::max_align_t), 0u);
    std::memset(p, 0xAB, size);  // must be writable across the whole request
    pool.Free(p);
  }
}

TEST(Alignment, LargeAllocationsArePageAligned) {
  Pool pool;
  void* p = pool.Allocate(3u << 20);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 4096, 0u);
  std::memset(p, 1, 3u << 20);
  pool.Free(p);
  // Default policy caches the mapping until the threshold or a Trim().
  EXPECT_EQ(pool.GetStats().committed_bytes, 3u << 20);
  EXPECT_EQ(pool.Trim(), 3u << 20);
  EXPECT_EQ(pool.GetStats().committed_bytes, 0u);
}

TEST(Reuse, LargeMappingsAreCachedAndReused) {
  Pool pool;
  void* a = pool.Allocate(1400000);
  pool.Free(a);
  void* b = pool.Allocate(1380000);  // rounds to a slightly smaller mapping: reuse
  EXPECT_EQ(a, b);
  pool.Free(b);
  void* c = pool.Allocate(4u << 20);  // far bigger: needs a fresh mapping
  EXPECT_NE(c, a);
  pool.Free(c);
}

TEST(Reuse, ImmediatePolicyUnmapsLargeBlocks) {
  Config cfg;
  cfg.policy = bufpool::ReturnPolicy::kImmediate;
  Pool pool(cfg);
  pool.Free(pool.Allocate(2u << 20));
  EXPECT_EQ(pool.GetStats().committed_bytes, 0u);
  EXPECT_EQ(pool.GetStats().returned_bytes, 2u << 20);
}

TEST(Reuse, FreedSlotIsHandedOutAgain) {
  Pool pool;
  void* a = pool.Allocate(1000);
  pool.Free(a);
  void* b = pool.Allocate(900);  // same 1 KiB class
  EXPECT_EQ(a, b);
  pool.Free(b);
}

TEST(Reuse, DistinctLivePointers) {
  Pool pool;
  std::set<void*> seen;
  std::vector<void*> ptrs;
  for (int i = 0; i < 5000; ++i) {
    void* p = pool.Allocate(512);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(seen.insert(p).second);
    ptrs.push_back(p);
  }
  // 5000 * 512 B spans two 2 MiB arenas.
  EXPECT_EQ(pool.GetStats().live_allocs, 5000u);
  for (void* p : ptrs) pool.Free(p);
  EXPECT_EQ(pool.GetStats().live_allocs, 0u);
  EXPECT_EQ(pool.GetStats().in_use_bytes, 0u);
}

TEST(Stats, TracksInUseAndPeak) {
  Pool pool({0, bufpool::ReturnPolicy::kNever, 0, false});
  void* a = pool.Allocate(64 * 1024);
  void* b = pool.Allocate(64 * 1024);
  auto s = pool.GetStats();
  EXPECT_EQ(s.in_use_bytes, 128u * 1024);
  EXPECT_EQ(s.committed_bytes, 128u * 1024);
  EXPECT_DOUBLE_EQ(s.fragmentation(), 0.0);
  pool.Free(a);
  s = pool.GetStats();
  EXPECT_EQ(s.in_use_bytes, 64u * 1024);
  EXPECT_EQ(s.peak_in_use_bytes, 128u * 1024);
  EXPECT_NEAR(s.fragmentation(), 0.5, 1e-9);  // kNever keeps the freed slot committed
  pool.Free(b);
}
