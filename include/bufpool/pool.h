// bufpool: a size-class pool for media-sized buffers under a hard memory budget.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bufpool {

// When to hand fully free pages back to the kernel.
enum class ReturnPolicy {
  kImmediate,       // madvise as soon as a page has no live slots
  kAboveThreshold,  // madvise once free-but-committed bytes exceed Config::return_threshold
  kNever,           // keep everything committed (only trimmed under budget pressure)
};

struct Config {
  // Hard limit on committed bytes. 0 means unlimited.
  size_t budget_bytes = 0;
  ReturnPolicy policy = ReturnPolicy::kAboveThreshold;
  size_t return_threshold = 8u << 20;
  // Poison freed memory and report double / invalid frees.
  bool debug = false;
};

struct Stats {
  size_t committed_bytes = 0;  // pages we believe are resident
  size_t in_use_bytes = 0;     // bytes handed out (rounded to slot size)
  size_t peak_committed_bytes = 0;
  size_t peak_in_use_bytes = 0;
  size_t failed_allocs = 0;
  size_t returned_bytes = 0;  // total bytes given back with madvise/munmap
  size_t madvise_calls = 0;
  size_t double_frees = 0;
  size_t invalid_frees = 0;
  size_t live_allocs = 0;
  // 1 - in_use / committed. 0 when nothing is committed.
  double fragmentation() const {
    return committed_bytes ? 1.0 - double(in_use_bytes) / double(committed_bytes) : 0.0;
  }
};

enum class FreeError { kDoubleFree, kInvalidPointer };
using ErrorHandler = std::function<void(FreeError, const void*)>;

constexpr size_t kMinClassShift = 8;   // 256 B
constexpr size_t kMaxClassShift = 20;  // 1 MiB
constexpr size_t kNumClasses = kMaxClassShift - kMinClassShift + 1;
constexpr size_t kArenaShift = 21;  // 2 MiB
constexpr size_t kArenaSize = size_t{1} << kArenaShift;
constexpr uint8_t kPoisonByte = 0xDD;

// Returns the size-class index for a request, or -1 if it needs its own mapping.
int SizeClassFor(size_t size);
size_t ClassSize(int cls);

class Pool {
 public:
  explicit Pool(Config cfg = {});
  ~Pool();
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;

  // Returns nullptr (and bumps failed_allocs) if the budget would be exceeded.
  void* Allocate(size_t size);
  void Free(void* p);

  // Hands every fully free committed page back to the OS. Returns bytes released.
  size_t Trim();

  Stats GetStats() const;
  const Config& config() const { return cfg_; }

  // In debug mode, called on double/invalid frees. Default prints and aborts.
  void SetErrorHandler(ErrorHandler h);

 private:
  struct Arena;
  struct SizeClass;

  void* AllocateLarge(size_t size);
  bool FreeLarge(void* p);
  size_t DropLargeCache();
  Arena* NewArena(int cls);
  Arena* FindArena(const void* p) const;
  bool ChargeCommit(size_t bytes);
  void Uncharge(size_t bytes);
  size_t ReleaseEmptyPages(SizeClass& sc);
  void ReportError(FreeError e, const void* p);
  void NoteInUse(long delta);

  Config cfg_;
  std::array<std::unique_ptr<SizeClass>, kNumClasses> classes_;

  // Two-level radix map from 2 MiB-aligned address to its arena. Readers are lock-free.
  static constexpr size_t kL1Bits = 13;
  static constexpr size_t kL2Bits = 48 - kArenaShift - kL1Bits;
  struct Leaf { std::atomic<Arena*> slots[size_t{1} << kL2Bits]; };
  std::unique_ptr<std::atomic<Leaf*>[]> radix_;

  std::mutex large_mu_;
  std::unordered_map<void*, size_t> large_;
  std::multimap<size_t, void*> large_cache_;  // freed big mappings kept for reuse

  std::atomic<size_t> committed_{0};
  std::atomic<size_t> in_use_{0};
  std::atomic<size_t> peak_committed_{0};
  std::atomic<size_t> peak_in_use_{0};
  std::atomic<size_t> free_committed_{0};  // committed pages with no live slots
  std::atomic<size_t> failed_{0};
  std::atomic<size_t> returned_{0};
  std::atomic<size_t> madvise_calls_{0};
  std::atomic<size_t> double_frees_{0};
  std::atomic<size_t> invalid_frees_{0};
  std::atomic<size_t> live_{0};

  std::mutex handler_mu_;
  ErrorHandler handler_;
};

}  // namespace bufpool
