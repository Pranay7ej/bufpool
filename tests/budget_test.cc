#include <gtest/gtest.h>

#include <sys/mman.h>
#include <unistd.h>

#include <vector>

#include "bufpool/pool.h"

using bufpool::Config;
using bufpool::Pool;
using bufpool::ReturnPolicy;

namespace {

Config Make(size_t budget, ReturnPolicy policy, size_t threshold = 0) {
  Config c;
  c.budget_bytes = budget;
  c.policy = policy;
  c.return_threshold = threshold;
  return c;
}

// Number of resident pages in [p, p + len), via mincore().
size_t ResidentPages(void* p, size_t len) {
  const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  std::vector<unsigned char> vec((len + ps - 1) / ps);
  if (mincore(p, len, vec.data()) != 0) return 0;
  size_t n = 0;
  for (unsigned char v : vec) n += v & 1;
  return n;
}

}  // namespace

TEST(Budget, RefusesPastTheLimit) {
  Pool pool(Make(1 << 20, ReturnPolicy::kNever));
  std::vector<void*> ptrs;
  for (int i = 0; i < 16; ++i) {
    void* p = pool.Allocate(64 * 1024);
    ASSERT_NE(p, nullptr) << i;
    ptrs.push_back(p);
  }
  EXPECT_EQ(pool.Allocate(64 * 1024), nullptr);
  EXPECT_EQ(pool.Allocate(4 << 20), nullptr);  // large path is budgeted too
  auto s = pool.GetStats();
  EXPECT_EQ(s.failed_allocs, 2u);
  EXPECT_LE(s.committed_bytes, 1u << 20);
  for (void* p : ptrs) pool.Free(p);
}

TEST(Budget, FreeingMakesRoomAgain) {
  Pool pool(Make(256 * 1024, ReturnPolicy::kImmediate));
  void* a = pool.Allocate(128 * 1024);
  void* b = pool.Allocate(128 * 1024);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(pool.Allocate(4096), nullptr);
  pool.Free(a);
  EXPECT_NE(pool.Allocate(4096), nullptr);
}

TEST(Budget, TrimsOtherClassesBeforeFailing) {
  // kNever leaves freed 64 KiB slots committed; a 128 KiB request must reclaim them.
  Pool pool(Make(256 * 1024, ReturnPolicy::kNever));
  void* a = pool.Allocate(64 * 1024);
  void* b = pool.Allocate(64 * 1024);
  void* c = pool.Allocate(128 * 1024);
  ASSERT_NE(c, nullptr);
  pool.Free(a);
  pool.Free(b);
  EXPECT_EQ(pool.GetStats().committed_bytes, 256u * 1024);
  void* d = pool.Allocate(128 * 1024);
  EXPECT_NE(d, nullptr);
  EXPECT_EQ(pool.GetStats().failed_allocs, 0u);
  EXPECT_GE(pool.GetStats().returned_bytes, 128u * 1024);
}

TEST(Return, ImmediateReleasesPagesOnFree) {
  Pool pool(Make(0, ReturnPolicy::kImmediate));
  const size_t n = 256 * 1024;
  char* p = static_cast<char*>(pool.Allocate(n));
  for (size_t i = 0; i < n; i += 4096) p[i] = 1;
  EXPECT_EQ(ResidentPages(p, n), n / 4096);
  pool.Free(p);
  EXPECT_EQ(ResidentPages(p, n), 0u);
  auto s = pool.GetStats();
  EXPECT_EQ(s.committed_bytes, 0u);
  EXPECT_EQ(s.returned_bytes, n);
}

TEST(Return, SmallSlotsOnlyReleaseFullyFreePages) {
  Pool pool(Make(0, ReturnPolicy::kImmediate));
  // 16 x 256 B share one 4 KiB page.
  std::vector<void*> ptrs;
  for (int i = 0; i < 16; ++i) ptrs.push_back(pool.Allocate(256));
  for (int i = 0; i < 15; ++i) pool.Free(ptrs[i]);
  EXPECT_EQ(pool.GetStats().committed_bytes, 4096u);
  EXPECT_EQ(pool.GetStats().returned_bytes, 0u);
  pool.Free(ptrs[15]);
  EXPECT_EQ(pool.GetStats().committed_bytes, 0u);
}

TEST(Return, NeverKeepsPagesUntilTrim) {
  Pool pool(Make(0, ReturnPolicy::kNever));
  const size_t n = 512 * 1024;
  char* p = static_cast<char*>(pool.Allocate(n));
  for (size_t i = 0; i < n; i += 4096) p[i] = 1;
  pool.Free(p);
  EXPECT_EQ(pool.GetStats().committed_bytes, n);
  EXPECT_EQ(ResidentPages(p, n), n / 4096);
  EXPECT_EQ(pool.Trim(), n);
  EXPECT_EQ(pool.GetStats().committed_bytes, 0u);
  EXPECT_EQ(ResidentPages(p, n), 0u);
}

TEST(Return, ThresholdWaitsForEnoughFreeMemory) {
  Pool pool(Make(0, ReturnPolicy::kAboveThreshold, 1 << 20));
  std::vector<void*> ptrs;
  for (int i = 0; i < 24; ++i) ptrs.push_back(pool.Allocate(64 * 1024));
  // Free 12 x 64 KiB = 768 KiB: still under the 1 MiB threshold.
  for (int i = 0; i < 12; ++i) pool.Free(ptrs[i]);
  EXPECT_EQ(pool.GetStats().returned_bytes, 0u);
  // Crossing 1 MiB triggers a sweep of everything free.
  for (int i = 12; i < 17; ++i) pool.Free(ptrs[i]);
  auto s = pool.GetStats();
  EXPECT_EQ(s.returned_bytes, 17u * 64 * 1024);
  EXPECT_EQ(s.committed_bytes, 7u * 64 * 1024);
  for (int i = 17; i < 24; ++i) pool.Free(ptrs[i]);
}

TEST(Return, RevivedPagesAreReusedWithoutRecharging) {
  Pool pool(Make(0, ReturnPolicy::kNever));
  void* a = pool.Allocate(64 * 1024);
  pool.Free(a);
  const size_t committed = pool.GetStats().committed_bytes;
  void* b = pool.Allocate(64 * 1024);
  EXPECT_EQ(a, b);
  EXPECT_EQ(pool.GetStats().committed_bytes, committed);
  pool.Free(b);
}
