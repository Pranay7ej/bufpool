#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "bufpool/pool.h"

using bufpool::Config;
using bufpool::FreeError;
using bufpool::Pool;
using bufpool::ReturnPolicy;

TEST(Threads, ConcurrentAllocFreeKeepsAccountingExact) {
  Config cfg;
  cfg.policy = ReturnPolicy::kAboveThreshold;
  cfg.return_threshold = 1 << 20;
  Pool pool(cfg);
  constexpr int kThreads = 8;
  constexpr int kIters = 20000;
  std::atomic<int> corrupt{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(1234 + t);
      std::uniform_int_distribution<size_t> size_dist(16, 256 * 1024);
      std::vector<std::pair<unsigned char*, size_t>> live;
      for (int i = 0; i < kIters; ++i) {
        if (live.empty() || (rng() % 3 != 0 && live.size() < 64)) {
          size_t n = size_dist(rng);
          auto* p = static_cast<unsigned char*>(pool.Allocate(n));
          if (!p) continue;
          std::memset(p, t + 1, n);
          live.emplace_back(p, n);
        } else {
          size_t idx = rng() % live.size();
          auto [p, n] = live[idx];
          // Another thread writing into our block would show up here.
          if (p[0] != t + 1 || p[n - 1] != t + 1) corrupt.fetch_add(1);
          pool.Free(p);
          live[idx] = live.back();
          live.pop_back();
        }
      }
      for (auto [p, n] : live) pool.Free(p);
    });
  }
  for (auto& th : threads) th.join();

  auto s = pool.GetStats();
  EXPECT_EQ(corrupt.load(), 0);
  EXPECT_EQ(s.live_allocs, 0u);
  EXPECT_EQ(s.in_use_bytes, 0u);
  EXPECT_EQ(s.double_frees, 0u);
  pool.Trim();
  EXPECT_EQ(pool.GetStats().committed_bytes, 0u);
}

TEST(Threads, BudgetHoldsUnderContention) {
  Config cfg;
  cfg.budget_bytes = 4 << 20;
  cfg.policy = ReturnPolicy::kImmediate;
  Pool pool(cfg);
  std::atomic<size_t> max_seen{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 6; ++t) {
    threads.emplace_back([&] {
      std::vector<void*> mine;
      for (int i = 0; i < 5000; ++i) {
        if (void* p = pool.Allocate(128 * 1024)) mine.push_back(p);
        size_t c = pool.GetStats().committed_bytes;
        size_t prev = max_seen.load();
        while (c > prev && !max_seen.compare_exchange_weak(prev, c)) {
        }
        if (mine.size() > 4) {
          pool.Free(mine.front());
          mine.erase(mine.begin());
        }
      }
      for (void* p : mine) pool.Free(p);
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_LE(max_seen.load(), 4u << 20);
  EXPECT_LE(pool.GetStats().peak_committed_bytes, 4u << 20);
}

TEST(Debug, PoisonsFreedMemory) {
  Config cfg;
  cfg.debug = true;
  cfg.policy = ReturnPolicy::kNever;  // keep the page so we can look at it
  Pool pool(cfg);
  auto* p = static_cast<unsigned char*>(pool.Allocate(1024));
  std::memset(p, 0x11, 1024);
  pool.Free(p);
  for (int i = 0; i < 1024; ++i) ASSERT_EQ(p[i], bufpool::kPoisonByte) << i;
}

TEST(Debug, DetectsDoubleFree) {
  Config cfg;
  cfg.debug = true;
  Pool pool(cfg);
  std::vector<FreeError> errors;
  pool.SetErrorHandler([&](FreeError e, const void*) { errors.push_back(e); });
  void* p = pool.Allocate(4096);
  void* keep = pool.Allocate(4096);
  pool.Free(p);
  pool.Free(p);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0], FreeError::kDoubleFree);
  EXPECT_EQ(pool.GetStats().double_frees, 1u);
  // The bad free must not corrupt accounting or the free list.
  EXPECT_EQ(pool.GetStats().live_allocs, 1u);
  void* a = pool.Allocate(4096);
  void* b = pool.Allocate(4096);
  EXPECT_NE(a, b);
  pool.Free(a);
  pool.Free(b);
  pool.Free(keep);
}

TEST(Debug, DetectsInvalidPointers) {
  Config cfg;
  cfg.debug = true;
  Pool pool(cfg);
  std::vector<FreeError> errors;
  pool.SetErrorHandler([&](FreeError e, const void*) { errors.push_back(e); });
  auto* p = static_cast<char*>(pool.Allocate(1024));
  pool.Free(p + 16);  // interior pointer
  int on_stack = 0;
  pool.Free(&on_stack);  // never came from the pool
  ASSERT_EQ(errors.size(), 2u);
  EXPECT_EQ(errors[0], FreeError::kInvalidPointer);
  EXPECT_EQ(errors[1], FreeError::kInvalidPointer);
  pool.Free(p);
  EXPECT_EQ(pool.GetStats().live_allocs, 0u);
}

TEST(Debug, ReleaseModeCountsButIgnoresDoubleFree) {
  Pool pool;  // debug off: no handler call, still safe
  void* p = pool.Allocate(2048);
  pool.Free(p);
  pool.Free(p);
  EXPECT_EQ(pool.GetStats().double_frees, 1u);
  EXPECT_EQ(pool.GetStats().live_allocs, 0u);
}
