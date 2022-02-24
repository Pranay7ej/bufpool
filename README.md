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
`/proc/self/statm`.

Traces in `bench/traces/`:

- `steady`, `seeky`, `abr` come from `bench/gen_trace.py`, which fakes a
  player's heap (network chunks, packets, decoded frames) for 10 minutes:
  one quality with a full buffer; a seek every 20 s that drops and refills
  everything; quality changes every 15 s so the dominant sizes keep moving
- `tinyplayer_drop`, `tinyplayer_fluct` are recorded from
  [tinyplayer](https://github.com/Pranay7ej/tinyplayer)'s SourceBuffer
  (`--alloc-trace`): only the downloaded segment buffers, 0.4–3 MB each, over
  10 minutes of playback on two network traces

```sh
./build/bufpool_bench bench/traces/*.txt
./build/bufpool_bench --budget 24 bench/traces/abr.txt   # with a hard cap
```

Results on a 2-core Xeon VM, no budget. RSS is sampled ~4k times per run.

| trace | allocator | ns/op | peak RSS MB | avg RSS MB | RSS after free MB |
|---|---|---:|---:|---:|---:|
| steady | malloc | 178 | 15.9 | 15.1 | 15.9 |
| steady | pool/immediate | 4388 | 17.6 | 15.7 | 0.7 |
| steady | pool/threshold | 205 | 20.6 | 19.3 | 0.7 |
| steady | pool/never | 177 | 20.6 | 19.3 | 20.6 |
| seeky | malloc | 164 | 14.4 | 11.1 | 9.4 |
| seeky | pool/immediate | 3450 | 15.6 | 9.2 | 0.8 |
| seeky | pool/threshold | 250 | 16.9 | 10.8 | 2.0 |
| seeky | pool/never | 125 | 17.8 | 16.6 | 17.8 |
| abr | malloc | 222 | 36.2 | 28.3 | 28.4 |
| abr | pool/immediate | 5872 | 35.5 | 19.1 | 1.1 |
| abr | pool/threshold | 341 | 40.9 | 23.3 | 6.3 |
| abr | pool/never | 162 | 60.4 | 49.9 | 60.4 |
| tinyplayer_drop | malloc | 4071 | 13.6 | 10.7 | 3.1 |
| tinyplayer_drop | pool/immediate | 25350 | 13.9 | 10.1 | 0.9 |
| tinyplayer_drop | pool/threshold | 14861 | 20.9 | 13.5 | 7.9 |
| tinyplayer_drop | pool/never | 1001 | 42.8 | 33.5 | 42.8 |
| tinyplayer_fluct | malloc | 2004 | 18.1 | 13.3 | 0.0 |
| tinyplayer_fluct | pool/immediate | 14771 | 18.9 | 12.6 | 0.9 |
| tinyplayer_fluct | pool/threshold | 5109 | 24.1 | 16.3 | 4.6 |
| tinyplayer_fluct | pool/never | 638 | 36.7 | 33.3 | 36.7 |

What I took away from it:

- **The pool doesn't beat malloc on peak memory.** Power-of-two classes waste
  space: a 10 KB packet sits in a 16 KB slot. On the steady trace that's ~30%
  more peak RSS. Finer classes (or a couple of classes tuned to your frame
  sizes) would fix most of it.
- **Where it wins is giving memory back.** On the synthetic traces malloc is
  still holding 9–28 MB after everything was freed (glibc only trims the top
  of the heap, and how much it keeps varies run to run). The pool with a
  return policy drops to ~1 MB.
- **Immediate return is way too expensive.** One `madvise` per free is ~20x
  slower per op, and that doesn't even count the page faults you pay when the
  memory gets reused. The threshold policy gets close on average RSS for about
  the cost of malloc.
- **Never returning is the worst of both** on anything with changing sizes:
  every quality you ever played stays resident, 60 MB for a player that needed
  ~36.
- **The tinyplayer traces are a different world.** They're all multi-MB
  segment buffers, so everything goes through the large-mapping path, and here
  glibc is already good: it `mmap`s big blocks and unmaps them on free. The
  pool's large-block cache only helps when sizes repeat, and segment sizes
  don't (VBR). If I were using this in the player I'd leave segments on
  malloc and pool the small stuff.
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
