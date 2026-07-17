# Chase-Lev Deque: `lastTop` Caching Optimization — Benchmark Results

## What was tested

cited directly from the paper:
Unlike the original ABP algorithm, the new algorithm requires reading top on every execution of the pushBottom
operation. This may result in more data-cache misses compared to the original algorithm (recall that unlike bottom,
top is modified by all processes).
The frequency of accesses to the top variable can be significantly reduced, by keeping a local upper bound on the
size of the deque, and only read top when the upper bound indicates that an array expansion may be necessary. Such a
local upper bound can be easily achieved by saving the last value of top read in a local variable, and using this variable to compute the size of the deque (instead of the real value of top). Because top is never decremented, the real size of the deque can only be smaller than the one calculated using this local variable.

This was implemented by using a non atomic variable `lastTop`. It is updated in these steps:
1) Right after reading `top` in `popBottom`
2) Right after `cas` in `popBottom`
3) There is an additional added check if `lastTop` indicates the the array needs to grow. There, `top` is read and `lastTop` is updated.

## Test setup

- **Hardware**: WSL2 (Ubuntu), 16 logical cores (`nproc`)
- **Compiler**: g++, `-pthread`
- **Workload**: single owner thread pushing sequential `int` tasks; N thief threads
  concurrently calling `steal()`
- **Correctness check**: every run verified 0 duplicates and 0 missing tasks
- **Metric**: wall-clock time for the owner's full push loop, via `std::chrono`
- **Methodology**: 50 runs per configuration; first 5 runs of each set discarded as
  warm-up noise; median reported (median chosen over mean because a handful of runs
  in every set showed OS-scheduling-induced spikes 2–3x the typical value — median is
  far less sensitive to those outliers than the mean)

## Results

### Configuration 1: 4 thief threads, 10,000 tasks

| Version | Median (50 runs, first 5 dropped) | us/task |
|---|---|---|
| Uncached (`top.load()` every push) | 4,590 us | 0.459 |
| Cached (`lastTop`) | 4,370 us | 0.437 |

**Cached version: ~4.8% faster.**

### Configuration 2: 12 thief threads, 100,000 tasks

| Version | Run | Median (50 runs, first 5 dropped) |
|---|---|---|
| Uncached | Run 1 | 83,568 us |
| Uncached | Run 2 | 83,464 us |
| Cached | Run 1 | 82,000 us |
| Cached | Run 2 | 82,699 us |

**Cached version: ~1–2.5% faster** — consistently below both uncached runs across
repeated trials, but a noticeably smaller margin than at lower contention.

## Interpretation

The optimization produces a real, directionally consistent speedup in every
configuration tested — cached never came out slower than uncached in any run — but
the magnitude shrinks as thread count and task count scale up (from ~5% at 4
threads/10k tasks to ~1–2% at 12 threads/100k tasks).

The most likely explaination: These benchmarks were run on WSL2 (Windows Subsystem for Linux), not bare-metal. WSL2 adds its own virtualization/scheduling overhead, which becomes more noticeable as thread contention increases. This overhead can mask or shrink the measured benefit of this technique, since the technique's savings come from reducing cache-coherence traffic — a relatively small effect that's easier to see when there's less other noise in the system. On bare-metal, many-core hardware (closer to what the original paper used), the performance gap is likely to be larger and more consistent.

## A correctness lesson from implementing this

In the initial implementation, when lastTop signaled that the array needed to grow, the code proceeded to grow it without first re-checking the actual, current value of top. In other words, it trusted the cached/stale lastTop value as sufficient justification to trigger a resize, rather than confirming against the real, up-to-date state of top before committing to that expensive operation.
This mattered because lastTop could be out of date — the real top might have already changed (for example, due to concurrent access, prior operations, or timing) by the time the growth check ran. Since there was no verification step comparing lastTop against the true current top, the implementation ended up growing the array more often than necessary, including in cases where a fresh check would have shown that growth wasn't actually needed yet.
Because array growth is a relatively expensive operation (allocating new memory, copying existing elements over, etc.), triggering it unnecessarily — or more frequently than required — introduced significant overhead. This unnecessary/excessive growing became a major performance bottleneck, resulting in the "huge grow overhead" observed.
