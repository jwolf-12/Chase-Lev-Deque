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


# Chase-Lev Deque: Shrink Logic — Memory Footprint Benchmark

Unlike the `lastTop` caching optimization, shrinking is not a speed optimization —
every shrink event costs real work (allocate a smaller array, copy live elements,
perform the bottom/top-shift synchronization the paper proves safe against concurrent
thieves, free the old buffer). The benefit is memory footprint, not throughput: an
array that grows to accommodate a burst of work should give that memory back once the
burst drains, rather than holding onto peak-sized memory forever.

## Test setup

- **Hardware**: WSL2 (Ubuntu), 16 logical cores
- **Compiler**: g++, `-O2 -pthread`
- **Workload**: an oscillating push/drain pattern — 20 bursts, each pushing 2,000
  tasks then draining 1,900 via `popBottom` — with 4 thief threads also stealing
  concurrently throughout. This exercises `perhapsShrink` repeatedly, unlike a
  monotonic push test where the array only ever grows.
- **Instrumentation**: `array_size`, `growCount`, and `shrinkCount` logged after every
  burst
- **Comparison**: same workload run once with shrink logic active, once with it
  disabled (`perhapsShrink` as a no-op) — 30 runs each
- **Correctness check**: every run verified 0 duplicates and 0 missing tasks

## Result

**Without shrink**: across all 30 runs, `array_size` reached 2048 early and stayed
flat at 2048 for every remaining burst, with `shrinks=0` throughout, every single
run — zero variance. Once the array grows to accommodate the peak backlog, that
memory is held for the rest of the program's life regardless of how much the deque
later drains.

**With shrink**: `array_size` visibly oscillates in nearly every run — climbing to
2048 during push bursts, dropping to 256–1024 (occasionally as low as 256) during
drain phases, then climbing back on the next burst. `growCount` and `shrinkCount`
climb together across bursts, roughly tracking each other, confirming the array is
actively responding to the workload's shape rather than shrinking as a rare fluke.

Example (single representative run, with shrink):
```
burst 0  array_size=2048 grows=7  shrinks=0
burst 1  array_size=1024 grows=7  shrinks=1
burst 2  array_size=2048 grows=8  shrinks=1
burst 5  array_size=512  grows=9  shrinks=4
burst 11 array_size=512  grows=10 shrinks=5
burst 19 array_size=2048 grows=17 shrinks=10
```
Compare to any without-shrink run, which reads `array_size=2048` on every single
line.

**Speed**: median owner-loop time was in the same rough range for both
configurations (~8,000–11,000 us for 40,000 total push+pop operations) — no
measurable speed advantage either direction, consistent with the expectation that
shrink trades a small per-event cost for memory reclamation rather than improving
throughput.

## Interpretation

Peak memory usage is identical between the two versions (both reach 2048 slots
during the burst). The difference is steady-state / average memory: the
shrink-enabled version spends much of its time at a fraction of peak size, while the
no-shrink version is permanently pinned at peak from the first burst onward. This is
exactly the trade-off the paper motivates: for `n` concurrent deques sharing `m`
total bytes of memory, a deque that never gives memory back after a burst wastes
capacity that other deques might need during the same period — shrinking lets each
deque's footprint track its actual current backlog instead of its historical peak.

