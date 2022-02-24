# bufpool

A small allocator for programs that churn through lots of similarly sized buffers
and have to live under a hard memory cap. Think a video player on a cheap TV
stick: network chunks, demuxed packets and decoded frames coming and going all
the time, and the OOM killer waiting if you go over.

malloc is fine at speed but it has no idea you have a budget, and it's lazy about
giving memory back. I wanted to see how much a dumb size-class pool with an
explicit "return pages to the OS" policy actually buys you.

## How it works

- Size classes are powers of two from 256 B to 1 MiB. Bigger requests get their
  own `mmap` (and freed ones are kept around for reuse unless the policy says
  otherwise, since video frames come in a few fixed sizes).
- Small classes are carved out of 2 MiB arenas. Arenas are 2 MiB aligned, so
  going from a pointer back to its arena is a shift plus a two-level table
  lookup, no locks.
- All the bookkeeping (free list, live bitmap, per-page counters) lives outside
  the arena. That matters: `MADV_DONTNEED` zeroes the pages, so an intrusive
  free list would get wiped.
- **Hard budget**: committed bytes can't go over `budget_bytes`. When they would,
  the pool first trims free pages from every class, then returns `nullptr` and
  bumps `failed_allocs`. The process never grows past the cap.
- **Return policies**
  - `kImmediate` — `madvise(MADV_DONTNEED)` as soon as a page has nothing live
  - `kAboveThreshold` — wait until free-but-committed memory passes a threshold
    (8 MiB default), then sweep
  - `kNever` — keep everything (only trimmed under budget pressure)
- One mutex per size class.
- Debug mode poisons freed memory with `0xDD` and reports double frees and
  frees of pointers that didn't come from the pool.

```cpp
bufpool::Config cfg;
cfg.budget_bytes = 64 << 20;
cfg.policy = bufpool::ReturnPolicy::kAboveThreshold;
bufpool::Pool pool(cfg);

void* seg = pool.Allocate(512 * 1024);
if (!seg) { /* over budget, drop something and retry */ }
pool.Free(seg);

auto s = pool.GetStats();  // committed, in use, peak, fragmentation(), failed...
```

## Build

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```

`-DBUFPOOL_SANITIZE=thread` (or `address`) builds everything with a sanitizer;
CI runs all three.

## Benchmark

`bufpool_bench` replays allocation traces against glibc malloc and each return
policy. Every run happens in a forked child so RSS doesn't leak between runs, and
every allocation touches its pages like a real producer would. RSS comes from
`/proc/self/statm`, sampled every 512 ops.

Traces in `bench/traces/` come from `bench/gen_trace.py`, which fakes a player's
heap for 10 minutes of playback:

- `steady` — one quality, buffer sitting near full
- `seeky` — a seek every 20 s, so everything buffered gets dropped and refilled
- `abr` — quality changes every 15 s, so the dominant sizes keep moving

```sh
./build/bufpool_bench bench/traces/*.txt
./build/bufpool_bench --budget 24 bench/traces/abr.txt   # with a hard cap
```

Results on a 2-core Xeon VM, no budget:

| trace | allocator | ns/op | peak RSS MB | avg RSS MB | RSS after free MB |
|---|---|---:|---:|---:|---:|
| steady | malloc | 146 | 16.9 | 15.7 | 14.7 |
| steady | pool/immediate | 5060 | 17.7 | 15.8 | 0.8 |
| steady | pool/threshold | 187 | 20.7 | 19.4 | 0.8 |
| steady | pool/never | 176 | 20.7 | 19.4 | 20.7 |
| seeky | malloc | 177 | 14.4 | 11.3 | 9.4 |
| seeky | pool/immediate | 4158 | 14.7 | 9.1 | 0.8 |
| seeky | pool/threshold | 272 | 16.9 | 10.7 | 2.0 |
| seeky | pool/never | 143 | 17.8 | 16.5 | 17.8 |
| abr | malloc | 192 | 33.7 | 27.3 | 13.8 |
| abr | pool/immediate | 5549 | 34.1 | 18.9 | 0.9 |
| abr | pool/threshold | 358 | 40.5 | 23.1 | 6.2 |
| abr | pool/never | 202 | 60.3 | 49.6 | 60.3 |

What I took away from it:

- **The pool doesn't beat malloc on peak memory.** Power-of-two classes waste
  space: a 10 KB packet sits in a 16 KB slot. On the steady trace that's ~20%
  more peak RSS. Finer classes (or a couple of classes tuned to your frame
  sizes) would fix most of it.
- **Where it wins is giving memory back.** After the burst is over, malloc is
  still holding 9–15 MB. The pool with a return policy drops to ~1 MB. On a
  device that's switching between apps, that's the number that matters.
- **Immediate return is way too expensive.** One `madvise` per free is ~25x
  slower per op, and it doesn't even count the page faults you pay when the
  memory gets reused. The threshold policy gets nearly the same average RSS for
  about the cost of malloc.
- **Never returning is the worst of both** on the ABR trace: every quality you
  ever played stays resident, 60 MB for a player that only needed ~34.
- With `--budget 24` the ABR trace needs more than 24 MB at 720p, so ~4.4k
  allocations fail instead of the process growing. That's the point, but it also
  means the caller has to actually handle `nullptr`.

## Tests

Alignment, reuse, stats, budget enforcement (including trimming other classes
before failing), all three return policies (checked with `mincore`, not just the
counters), an 8-thread stress test that checks nobody scribbles on anyone else's
block, and the debug-mode double/invalid free detection.

## Not done / ideas

- Arenas are never unmapped, only `DONTNEED`ed. Address space is cheap on 64-bit
  so I didn't bother.
- No per-thread caches. The per-class mutex was fine for a player with a few
  threads; it wouldn't be for a web server.
- Linux only (`mmap`/`madvise`/`mincore`).
