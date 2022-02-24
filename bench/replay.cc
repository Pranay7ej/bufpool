// Replays allocation traces against malloc and each bufpool return policy.
//
//   bufpool_bench [--budget MB] [--threshold MB] trace1.txt [trace2.txt ...]
//
// Every (trace, allocator) pair runs in a fresh child process so RSS numbers do
// not bleed between runs. Each allocation touches one byte per page, like a
// real producer filling the buffer, so pages actually become resident.
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "bufpool/pool.h"

namespace {

struct Op {
  bool alloc;
  uint32_t id;
  uint32_t size;
};

struct Result {
  double ns_per_op;
  double peak_rss_mb;   // sampled high-water mark above the pre-run baseline
  double final_rss_mb;  // after the whole trace was freed
  double avg_rss_mb;
  uint64_t failed;
};

std::vector<Op> LoadTrace(const char* path, uint32_t* max_id) {
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", path);
    std::exit(1);
  }
  std::vector<Op> ops;
  std::string line;
  *max_id = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    Op op{};
    char kind = line[0];
    std::istringstream ss(line.substr(1));
    ss >> op.id;
    op.alloc = kind == 'a';
    if (op.alloc) ss >> op.size;
    if (op.id > *max_id) *max_id = op.id;
    ops.push_back(op);
  }
  return ops;
}

double RssMb() {
  static const long page = sysconf(_SC_PAGESIZE);
  FILE* f = std::fopen("/proc/self/statm", "r");
  long size = 0, resident = 0;
  if (f) {
    if (std::fscanf(f, "%ld %ld", &size, &resident) != 2) resident = 0;
    std::fclose(f);
  }
  return double(resident) * double(page) / (1024.0 * 1024.0);
}

inline void Touch(void* p, size_t n) {
  auto* c = static_cast<volatile char*>(p);
  for (size_t i = 0; i < n; i += 4096) c[i] = 1;
  c[n - 1] = 1;
}

template <class AllocFn, class FreeFn>
Result Replay(const std::vector<Op>& ops, uint32_t max_id, AllocFn&& alloc, FreeFn&& release) {
  std::vector<void*> slots(max_id + 1, nullptr);
  const double base = RssMb();
  double peak = 0, sum = 0;
  size_t samples = 0;
  uint64_t failed = 0;
  std::chrono::nanoseconds busy{0};
  // ~4k samples per run, so short traces still get a real peak.
  const size_t kSampleEvery = std::max<size_t>(1, ops.size() / 4096);

  for (size_t i = 0; i < ops.size(); ++i) {
    const Op& op = ops[i];
    auto t0 = std::chrono::steady_clock::now();
    if (op.alloc) {
      void* p = alloc(op.size);
      busy += std::chrono::steady_clock::now() - t0;
      if (!p) {
        ++failed;
        continue;
      }
      Touch(p, op.size);
      slots[op.id] = p;
    } else {
      void* p = slots[op.id];
      if (p) release(p);
      busy += std::chrono::steady_clock::now() - t0;
      slots[op.id] = nullptr;
    }
    if (i % kSampleEvery == 0) {
      double r = RssMb() - base;
      if (r > peak) peak = r;
      sum += r;
      ++samples;
    }
  }
  // Recorded traces can end with buffers still live; free them so the last
  // column really is "after everything was freed".
  for (void*& p : slots)
    if (p) {
      release(p);
      p = nullptr;
    }
  Result res{};
  res.ns_per_op = double(busy.count()) / double(ops.size());
  res.peak_rss_mb = peak;
  res.avg_rss_mb = samples ? sum / double(samples) : 0;
  res.final_rss_mb = std::max(0.0, RssMb() - base);  // can dip below the baseline
  res.failed = failed;
  return res;
}

Result RunInChild(const std::vector<Op>& ops, uint32_t max_id, int which, size_t budget,
                  size_t threshold) {
  int fds[2];
  if (pipe(fds) != 0) std::exit(1);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    Result r{};
    if (which == 0) {
      r = Replay(ops, max_id, [](size_t n) { return std::malloc(n); },
                 [](void* p) { std::free(p); });
    } else {
      bufpool::Config cfg;
      cfg.budget_bytes = budget;
      cfg.return_threshold = threshold;
      cfg.policy = which == 1   ? bufpool::ReturnPolicy::kImmediate
                   : which == 2 ? bufpool::ReturnPolicy::kAboveThreshold
                                : bufpool::ReturnPolicy::kNever;
      bufpool::Pool pool(cfg);
      r = Replay(ops, max_id, [&](size_t n) { return pool.Allocate(n); },
                 [&](void* p) { pool.Free(p); });
    }
    if (write(fds[1], &r, sizeof r) != sizeof r) _exit(1);
    _exit(0);
  }
  close(fds[1]);
  Result r{};
  if (read(fds[0], &r, sizeof r) != sizeof r) std::fprintf(stderr, "child failed\n");
  close(fds[0]);
  waitpid(pid, nullptr, 0);
  return r;
}

const char* Basename(const char* p) {
  const char* s = std::strrchr(p, '/');
  return s ? s + 1 : p;
}

}  // namespace

int main(int argc, char** argv) {
  size_t budget = 0, threshold = 8u << 20;
  std::vector<const char*> traces;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--budget") && i + 1 < argc)
      budget = size_t(std::atof(argv[++i]) * 1024 * 1024);
    else if (!std::strcmp(argv[i], "--threshold") && i + 1 < argc)
      threshold = size_t(std::atof(argv[++i]) * 1024 * 1024);
    else
      traces.push_back(argv[i]);
  }
  if (traces.empty()) {
    std::fprintf(stderr, "usage: %s [--budget MB] [--threshold MB] trace...\n", argv[0]);
    return 1;
  }
  const char* names[] = {"malloc", "pool/immediate", "pool/threshold", "pool/never"};
  std::printf("| trace | allocator | ns/op | peak RSS MB | avg RSS MB | RSS after free MB | failed |\n");
  std::printf("|---|---|---:|---:|---:|---:|---:|\n");
  for (const char* path : traces) {
    uint32_t max_id = 0;
    auto ops = LoadTrace(path, &max_id);
    for (int which = 0; which < 4; ++which) {
      Result r = RunInChild(ops, max_id, which, budget, threshold);
      std::printf("| %s | %s | %.0f | %.1f | %.1f | %.1f | %llu |\n", Basename(path), names[which],
                  r.ns_per_op, r.peak_rss_mb, r.avg_rss_mb, r.final_rss_mb,
                  static_cast<unsigned long long>(r.failed));
      std::fflush(stdout);
    }
  }
  return 0;
}
