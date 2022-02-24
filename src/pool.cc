#include "bufpool/pool.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bufpool {
namespace {

size_t PageSize() {
  static const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  return ps;
}

void AtomicMax(std::atomic<size_t>& a, size_t v) {
  size_t cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

size_t RoundUp(size_t n, size_t to) { return (n + to - 1) / to * to; }

}  // namespace

int SizeClassFor(size_t size) {
  if (size > (size_t{1} << kMaxClassShift)) return -1;
  size_t shift = kMinClassShift;
  while ((size_t{1} << shift) < size) ++shift;
  return static_cast<int>(shift - kMinClassShift);
}

size_t ClassSize(int cls) { return size_t{1} << (cls + kMinClassShift); }

// One 2 MiB mapping carved into equal slots of a single size class. All metadata
// lives outside the mapping so MADV_DONTNEED can zero pages without losing state.
//
// Residency is tracked per "unit": one page for slots smaller than a page, or
// one whole slot for bigger ones. A slot never straddles units, so alloc/free
// touch exactly one counter no matter how large the slot is.
struct Pool::Arena {
  char* base = nullptr;
  int cls = 0;
  size_t slot_size = 0;
  size_t unit_bytes = 0;
  uint32_t nslots = 0;
  uint32_t slots_per_unit = 0;
  std::vector<uint64_t> live_bits;
  std::vector<uint32_t> free_slots;  // LIFO keeps recently used (warm) slots in play
  std::vector<uint16_t> unit_live;   // live slots in each unit
  std::vector<uint8_t> unit_committed;

  bool IsLive(uint32_t i) const { return live_bits[i / 64] >> (i % 64) & 1; }
  void SetLive(uint32_t i, bool v) {
    if (v)
      live_bits[i / 64] |= uint64_t{1} << (i % 64);
    else
      live_bits[i / 64] &= ~(uint64_t{1} << (i % 64));
  }
  size_t UnitOf(uint32_t slot) const { return slot / slots_per_unit; }
};

struct Pool::SizeClass {
  std::mutex mu;
  std::vector<Arena*> arenas;
};

Pool::Pool(Config cfg) : cfg_(cfg) {
  for (auto& c : classes_) c = std::make_unique<SizeClass>();
  radix_.reset(new std::atomic<Leaf*>[size_t{1} << kL1Bits]);
  for (size_t i = 0; i < (size_t{1} << kL1Bits); ++i) radix_[i].store(nullptr);
  handler_ = [](FreeError e, const void* p) {
    std::fprintf(stderr, "bufpool: %s at %p\n",
                 e == FreeError::kDoubleFree ? "double free" : "invalid free", p);
    std::abort();
  };
}

Pool::~Pool() {
  for (auto& sc : classes_) {
    for (Arena* a : sc->arenas) {
      munmap(a->base, kArenaSize);
      delete a;
    }
  }
  for (auto& [p, sz] : large_) munmap(p, sz);
  for (auto& [sz, p] : large_cache_) munmap(p, sz);
  for (size_t i = 0; i < (size_t{1} << kL1Bits); ++i) delete radix_[i].load();
}

void Pool::SetErrorHandler(ErrorHandler h) {
  std::lock_guard<std::mutex> lk(handler_mu_);
  handler_ = std::move(h);
}

void Pool::ReportError(FreeError e, const void* p) {
  (e == FreeError::kDoubleFree ? double_frees_ : invalid_frees_).fetch_add(1);
  if (!cfg_.debug) return;
  ErrorHandler h;
  {
    std::lock_guard<std::mutex> lk(handler_mu_);
    h = handler_;
  }
  if (h) h(e, p);
}

bool Pool::ChargeCommit(size_t bytes) {
  size_t cur = committed_.load(std::memory_order_relaxed);
  do {
    if (cfg_.budget_bytes && cur + bytes > cfg_.budget_bytes) return false;
  } while (!committed_.compare_exchange_weak(cur, cur + bytes, std::memory_order_relaxed));
  AtomicMax(peak_committed_, cur + bytes);
  return true;
}

void Pool::Uncharge(size_t bytes) { committed_.fetch_sub(bytes, std::memory_order_relaxed); }

void Pool::NoteInUse(long delta) {
  if (delta >= 0) {
    size_t now = in_use_.fetch_add(size_t(delta), std::memory_order_relaxed) + size_t(delta);
    AtomicMax(peak_in_use_, now);
  } else {
    in_use_.fetch_sub(size_t(-delta), std::memory_order_relaxed);
  }
}

Pool::Arena* Pool::NewArena(int cls) {
  // Over-map by one arena so we can carve out a 2 MiB aligned window; alignment
  // lets FindArena go from any interior pointer to its arena with a shift.
  const size_t span = kArenaSize * 2;
  void* raw = mmap(nullptr, span, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (raw == MAP_FAILED) return nullptr;
  uintptr_t start = reinterpret_cast<uintptr_t>(raw);
  uintptr_t aligned = (start + kArenaSize - 1) & ~(kArenaSize - 1);
  if (aligned > start) munmap(raw, aligned - start);
  uintptr_t tail = aligned + kArenaSize;
  if (start + span > tail) munmap(reinterpret_cast<void*>(tail), start + span - tail);

  auto* a = new Arena;
  a->base = reinterpret_cast<char*>(aligned);
  a->cls = cls;
  a->slot_size = ClassSize(cls);
  a->nslots = static_cast<uint32_t>(kArenaSize / a->slot_size);
  a->live_bits.assign((a->nslots + 63) / 64, 0);
  a->free_slots.reserve(a->nslots);
  for (uint32_t i = a->nslots; i-- > 0;) a->free_slots.push_back(i);
  a->unit_bytes = std::max(a->slot_size, PageSize());
  a->slots_per_unit = static_cast<uint32_t>(a->unit_bytes / a->slot_size);
  const size_t nunits = kArenaSize / a->unit_bytes;
  a->unit_live.assign(nunits, 0);
  a->unit_committed.assign(nunits, 0);

  const size_t key = aligned >> kArenaShift;
  const size_t l1 = key >> kL2Bits, l2 = key & ((size_t{1} << kL2Bits) - 1);
  assert(l1 < (size_t{1} << kL1Bits));
  Leaf* leaf = radix_[l1].load(std::memory_order_acquire);
  if (!leaf) {
    auto* fresh = new Leaf;
    for (auto& s : fresh->slots) s.store(nullptr, std::memory_order_relaxed);
    if (radix_[l1].compare_exchange_strong(leaf, fresh, std::memory_order_acq_rel)) {
      leaf = fresh;
    } else {
      delete fresh;  // another thread won; `leaf` now holds its table
    }
  }
  leaf->slots[l2].store(a, std::memory_order_release);
  return a;
}

Pool::Arena* Pool::FindArena(const void* p) const {
  const size_t key = reinterpret_cast<uintptr_t>(p) >> kArenaShift;
  const size_t l1 = key >> kL2Bits, l2 = key & ((size_t{1} << kL2Bits) - 1);
  if (l1 >= (size_t{1} << kL1Bits)) return nullptr;
  Leaf* leaf = radix_[l1].load(std::memory_order_acquire);
  return leaf ? leaf->slots[l2].load(std::memory_order_acquire) : nullptr;
}

void* Pool::Allocate(size_t size) {
  if (size == 0) size = 1;
  const int cls = SizeClassFor(size);
  if (cls < 0) return AllocateLarge(size);
  SizeClass& sc = *classes_[cls];

  for (int attempt = 0; attempt < 2; ++attempt) {
    {
      std::lock_guard<std::mutex> lk(sc.mu);
      Arena* a = nullptr;
      for (Arena* cand : sc.arenas) {
        if (!cand->free_slots.empty()) {
          a = cand;
          break;
        }
      }
      if (!a) {
        a = NewArena(cls);
        if (!a) break;
        sc.arenas.push_back(a);
      }
      const uint32_t slot = a->free_slots.back();
      const size_t u = a->UnitOf(slot);
      const size_t fresh = a->unit_committed[u] ? 0 : a->unit_bytes;
      if (fresh && !ChargeCommit(fresh)) {
        // Over budget; fall through and trim without holding our lock.
      } else {
        a->free_slots.pop_back();
        a->SetLive(slot, true);
        if (a->unit_committed[u] && a->unit_live[u] == 0)
          free_committed_.fetch_sub(a->unit_bytes, std::memory_order_relaxed);
        a->unit_committed[u] = 1;
        ++a->unit_live[u];
        NoteInUse(long(a->slot_size));
        live_.fetch_add(1, std::memory_order_relaxed);
        return a->base + size_t{slot} * a->slot_size;
      }
    }
    if (attempt == 0 && Trim() == 0) break;
  }
  failed_.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

void* Pool::AllocateLarge(size_t size) {
  const size_t want = RoundUp(size, PageSize());
  void* p = nullptr;
  size_t sz = want;
  {
    // Reuse a cached mapping if one is close in size (within 25%). Video frames
    // come in a handful of fixed sizes, so this hits almost every time.
    std::lock_guard<std::mutex> lk(large_mu_);
    auto it = large_cache_.lower_bound(want);
    if (it != large_cache_.end() && it->first <= want + want / 4) {
      sz = it->first;
      p = it->second;
      large_cache_.erase(it);
      free_committed_.fetch_sub(sz, std::memory_order_relaxed);
      large_[p] = sz;
    }
  }
  if (!p) {
    if (!ChargeCommit(sz) && (!Trim() || !ChargeCommit(sz))) {
      failed_.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
      Uncharge(sz);
      failed_.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    std::lock_guard<std::mutex> lk(large_mu_);
    large_[p] = sz;
  }
  NoteInUse(long(sz));
  live_.fetch_add(1, std::memory_order_relaxed);
  return p;
}

bool Pool::FreeLarge(void* p) {
  size_t sz;
  bool unmap = cfg_.policy == ReturnPolicy::kImmediate;
  {
    std::lock_guard<std::mutex> lk(large_mu_);
    auto it = large_.find(p);
    if (it == large_.end()) return false;
    sz = it->second;
    large_.erase(it);
    if (!unmap) {
      if (cfg_.debug) std::memset(p, kPoisonByte, sz);
      large_cache_.emplace(sz, p);
      free_committed_.fetch_add(sz, std::memory_order_relaxed);
    }
  }
  NoteInUse(-long(sz));
  live_.fetch_sub(1, std::memory_order_relaxed);
  if (unmap) {
    munmap(p, sz);
    Uncharge(sz);
    returned_.fetch_add(sz, std::memory_order_relaxed);
  } else if (cfg_.policy == ReturnPolicy::kAboveThreshold &&
             free_committed_.load(std::memory_order_relaxed) > cfg_.return_threshold) {
    Trim();
  }
  return true;
}

size_t Pool::DropLargeCache() {
  std::multimap<size_t, void*> victims;
  {
    std::lock_guard<std::mutex> lk(large_mu_);
    victims.swap(large_cache_);
  }
  size_t bytes = 0;
  for (auto& [sz, p] : victims) {
    munmap(p, sz);
    bytes += sz;
  }
  if (bytes) {
    free_committed_.fetch_sub(bytes, std::memory_order_relaxed);
    Uncharge(bytes);
    returned_.fetch_add(bytes, std::memory_order_relaxed);
    madvise_calls_.fetch_add(victims.size(), std::memory_order_relaxed);
  }
  return bytes;
}

void Pool::Free(void* p) {
  if (!p) return;
  Arena* a = FindArena(p);
  if (!a) {
    if (!FreeLarge(p)) ReportError(FreeError::kInvalidPointer, p);
    return;
  }
  SizeClass& sc = *classes_[a->cls];
  bool check_threshold = false;
  {
    std::unique_lock<std::mutex> lk(sc.mu);
    const size_t off = static_cast<size_t>(static_cast<char*>(p) - a->base);
    const uint32_t slot = static_cast<uint32_t>(off / a->slot_size);
    if (off % a->slot_size != 0 || !a->IsLive(slot)) {
      lk.unlock();  // the handler may want to call back into the pool
      ReportError(off % a->slot_size ? FreeError::kInvalidPointer : FreeError::kDoubleFree, p);
      return;
    }
    if (cfg_.debug) std::memset(p, kPoisonByte, a->slot_size);
    a->SetLive(slot, false);
    a->free_slots.push_back(slot);
    NoteInUse(-long(a->slot_size));
    live_.fetch_sub(1, std::memory_order_relaxed);

    const size_t u = a->UnitOf(slot);
    if (--a->unit_live[u] == 0) {
      if (cfg_.policy == ReturnPolicy::kImmediate) {
        madvise(a->base + u * a->unit_bytes, a->unit_bytes, MADV_DONTNEED);
        a->unit_committed[u] = 0;
        Uncharge(a->unit_bytes);
        returned_.fetch_add(a->unit_bytes, std::memory_order_relaxed);
        madvise_calls_.fetch_add(1, std::memory_order_relaxed);
      } else {
        free_committed_.fetch_add(a->unit_bytes, std::memory_order_relaxed);
      }
    }
    check_threshold = cfg_.policy == ReturnPolicy::kAboveThreshold &&
                      free_committed_.load(std::memory_order_relaxed) > cfg_.return_threshold;
  }
  if (check_threshold) Trim();
}

size_t Pool::ReleaseEmptyPages(SizeClass& sc) {
  size_t released = 0;
  for (Arena* a : sc.arenas) {
    const size_t n = a->unit_live.size();
    size_t u = 0;
    while (u < n) {
      if (!(a->unit_committed[u] && a->unit_live[u] == 0)) {
        ++u;
        continue;
      }
      size_t end = u;  // coalesce neighbours into one madvise call
      while (end < n && a->unit_committed[end] && a->unit_live[end] == 0) ++end;
      const size_t bytes = (end - u) * a->unit_bytes;
      madvise(a->base + u * a->unit_bytes, bytes, MADV_DONTNEED);
      std::fill(a->unit_committed.begin() + long(u), a->unit_committed.begin() + long(end), 0);
      madvise_calls_.fetch_add(1, std::memory_order_relaxed);
      released += bytes;
      u = end;
    }
  }
  if (released) {
    free_committed_.fetch_sub(released, std::memory_order_relaxed);
    Uncharge(released);
    returned_.fetch_add(released, std::memory_order_relaxed);
  }
  return released;
}

size_t Pool::Trim() {
  size_t total = DropLargeCache();
  for (auto& sc : classes_) {
    std::lock_guard<std::mutex> lk(sc->mu);
    total += ReleaseEmptyPages(*sc);
  }
  return total;
}

Stats Pool::GetStats() const {
  Stats s;
  s.committed_bytes = committed_.load();
  s.in_use_bytes = in_use_.load();
  s.peak_committed_bytes = peak_committed_.load();
  s.peak_in_use_bytes = peak_in_use_.load();
  s.failed_allocs = failed_.load();
  s.returned_bytes = returned_.load();
  s.madvise_calls = madvise_calls_.load();
  s.double_frees = double_frees_.load();
  s.invalid_frees = invalid_frees_.load();
  s.live_allocs = live_.load();
  return s;
}

}  // namespace bufpool
