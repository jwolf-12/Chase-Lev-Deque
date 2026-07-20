# Chase-Lev Work-Stealing Deque

While looking into what compare-and-swap actually is, I came across the Chase-Lev
lock-free work-stealing deque (Chase & Lev, 2005) as a real-world use of it, and
ended up implementing it in C++. One owner thread pushes and pops from the
"bottom" of the deque; any number of "thief" threads concurrently steal from the
"top." The owner's hot-path operations (`pushBottom`, `popBottom`) never take a
lock and, in the uncontended case, involve no atomic contention at all, while
thieves race each other and the owner using compare-and-swap rather than
blocking.

## Contents

- [Core design](#core-design)
- [Benchmark methodology](#benchmark-methodology)
- [1. Base algorithm & correctness](#1-base-algorithm--correctness)
- [2. Dynamic growth and shrinking](#2-dynamic-growth-and-shrinking)
- [3. `lastTop` caching optimization](#3-lasttop-caching-optimization)
- [4. Shrink threshold (K) sweep](#4-shrink-threshold-k-sweep)
- [5. Epoch-based reclamation](#5-epoch-based-reclamation)
- [6. Memory ordering](#6-memory-ordering)
- [7. ThreadSanitizer validation](#7-threadsanitizer-validation)
- [8. Parallel merge sort vs. mutex-based queue](#8-parallel-merge-sort-vs-mutex-based-queue)
- [9. SC fences (closing the IRIW gap)](#9-sc-fences-closing-the-iriw-gap)
- [10. Cache-line padding](#10-cache-line-padding)

## Core design

- **Fixed-shape circular buffer, dynamically resized.** `top` and `bottom` are
  atomic counters; the active array is swapped out (not resized in place) when it
  needs to grow or shrink, using the same array-doubling/halving strategy the
  original paper describes.
- **CAS-based contested-element resolution.** The one genuinely hard case in the
  whole algorithm — a thief and the owner racing over the single last element in
  the deque — is resolved by a compare-and-swap on `top`: whichever side's CAS
  succeeds legitimately owns that element, and the loser's speculative read is
  discarded.
- **Dynamic growth and shrinking**, following the paper's low-water-mark logic and
  the `size % array.size() == 0` sentinel thieves use to detect an in-progress
  shrink without misreporting the deque as non-empty.

## Benchmark methodology

Unless a section below says otherwise, all benchmarks share this setup:

- **Hardware**: WSL2 (Ubuntu), 16 logical cores
- **Compiler**: g++, `-std=c++17 -O2 -pthread`
- **Workload**: an oscillating push/drain pattern — 20 bursts, each pushing 2,000
  tasks then draining 1,900 via `popBottom` — with 4 thief threads also stealing
  concurrently throughout
- **Sample size**: 60 full program runs per configuration
- **Correctness check**: every run verified 0 duplicates and 0 missing tasks

Sections that used a different workload (the `lastTop` and merge sort benchmarks)
state their own setup inline.

*Note: everything here was measured on WSL2. WSL2's hypervisor layer adds
scheduling overhead relative to bare metal, so percentages below are treated as
lower-bound estimates — a native-Linux rerun would give a tighter number.*

## 1. Base algorithm & correctness

Atomic top/bottom, CAS-based steal, correct resolution of the contested
last-element race. An initial task-loss bug (3–10% of tasks silently dropped
under contention, no duplicates) was found via a randomized multithreaded
stress-testing harness and fixed — the read-before-CAS ordering in
`steal`/`popBottom` had been backwards.

## 2. Dynamic growth and shrinking

`perhapsShrink` lets the deque's memory footprint track its actual current
backlog instead of its historical peak, rather than only ever growing.

## 3. `lastTop` caching optimization

From the paper's own discussion of reducing contended reads of `top` on the
`pushBottom` hot path:

> Unlike the original ABP algorithm, the new algorithm requires reading top on
> every execution of the pushBottom operation. This may result in more data-cache
> misses compared to the original algorithm... The frequency of accesses to the
> top variable can be significantly reduced, by keeping a local upper bound on the
> size of the deque, and only read top when the upper bound indicates that an
> array expansion may be necessary.

Implemented via a non-atomic `lastTop`, updated: (1) right after reading `top` in
`popBottom`, (2) right after the CAS in `popBottom`, and (3) whenever `lastTop`
itself indicates the array may need to grow, at which point the real `top` is
read and `lastTop` refreshed.

### Test setup

- **Hardware**: WSL2 (Ubuntu), 16 logical cores
- **Compiler**: g++, `-pthread`
- **Workload**: single owner thread pushing sequential `int` tasks; N thief
  threads concurrently calling `steal()`
- **Methodology**: 50 runs per configuration, first 5 discarded as warm-up noise,
  median reported (median chosen over mean — a handful of runs in every set
  showed OS-scheduling-induced spikes 2–3× the typical value)
- **Correctness check**: every run verified 0 duplicates and 0 missing tasks

### Results — 4 thief threads, 10,000 tasks

| Version | Median (50 runs, first 5 dropped) | us/task |
|---|---|---|
| Uncached (`top.load()` every push) | 4,590 us | 0.459 |
| Cached (`lastTop`) | 4,370 us | 0.437 |

**Cached version: ~4.8% faster.**

### Results — 12 thief threads, 100,000 tasks

| Version | Run | Median (50 runs, first 5 dropped) |
|---|---|---|
| Uncached | Run 1 | 83,568 us |
| Uncached | Run 2 | 83,464 us |
| Cached | Run 1 | 82,000 us |
| Cached | Run 2 | 82,699 us |

**Cached version: ~1–2.5% faster** — consistently below both uncached runs across
repeated trials, but a noticeably smaller margin than at lower contention.

### Interpretation

The optimization produces a real, directionally consistent speedup in every
configuration tested — cached never came out slower than uncached in any run —
but the magnitude shrinks as thread count and task count scale up (from ~5% at 4
threads/10k tasks to ~1–2% at 12 threads/100k tasks). The likely explanation:
WSL2 adds virtualization/scheduling overhead that grows with thread contention,
which can mask a technique whose savings come from reduced cache-coherence
traffic — a relatively small effect that's easier to see with less other system
noise. On bare-metal, many-core hardware (closer to what the original paper
used), the gap is likely to be larger and more consistent.

### A correctness lesson from implementing this

An early version relied on `popBottom` incidentally refreshing the cache: when
`lastTop` signaled the array needed to grow, the code grew it without
re-checking the real, current value of `top` first — trusting the possibly-stale
`lastTop` as sufficient justification. Since `lastTop` could lag the true `top`
(due to concurrent steals or timing), this triggered array growth more often
than necessary. Because growth is expensive (allocate, copy, free), the
unnecessary regrowth became a real performance bottleneck. This passed every
test under one specific call pattern and only broke once a workload was built
that didn't happen to exercise that pattern.

## 4. Shrink threshold (K) sweep

`perhapsShrink`'s trigger condition is `size < array.size() / K` — a **smaller**
K makes the threshold larger, so the deque shrinks *more* aggressively; a
**larger** K is more conservative.

**A precision worth being explicit about**: the `array_size` metric below
measures the deque's own logical buffer size — how large the currently-active
`CircularArray` is — not total OS-level process memory. The claim this
benchmark supports is specifically that shrinking keeps the deque's own array
buffer sized to its current backlog instead of its historical peak, not a claim
about total process memory usage.

> **Note on data provenance:** an earlier pass of this benchmark was run before
> epoch-based reclamation (§5) was added to the shrink path. That data is
> discarded and superseded by the numbers here — epoch reclamation adds real
> per-steal and per-shrink bookkeeping, so pre-epoch throughput numbers aren't
> comparable to the current implementation.

Uses the shared [benchmark methodology](#benchmark-methodology) above, with
1,200 burst-size samples per configuration (60 runs × 20 bursts), and the
implementation under test is shrink logic with epoch-based reclamation active
(not the pre-epoch never-free version).

### Results

| Config | Avg throughput (us/task) | Avg array_size (buffer) | Buffer size vs. no-shrink |
|---|---|---|---|
| No shrink (baseline) | 0.0917 | 2228.9 | — |
| K=3 | 0.1069 | 663.1 | −70.3% |
| K=4 | 0.1027 | 771.9 | −65.4% |
| K=5 | 0.1037 | 896.9 | −59.8% |
| K=8 | 0.1034 | 937.1 | −58.0% |
| K=12 | 0.1035 | 1084.1 | −51.4% |

### Interpretation

**Array buffer size scales cleanly and predictably with K.** As K increases from
3 to 12, the shrink trigger gets stricter and average array size climbs
monotonically back toward the no-shrink baseline (663 → 772 → 897 → 938 → 1084,
approaching 2228.9).

**Throughput stays close to flat in absolute terms, with a small, consistent
relative gap.** Every shrink-enabled configuration lands in a 0.1027–0.1069
us/task band against a 0.0917 us/task no-shrink baseline. On the same 0–0.40
us/task scale used throughout this project's charts, that gap is easy to miss —
but in relative terms, shrink-enabled configs sit roughly 12–17% higher than
baseline. Whether that matters depends on how tight the actual throughput budget
is.

**Within the shrink-enabled configs, throughput barely moves with K.** K=4, 5,
8, and 12 all cluster tightly around 0.103–0.104 us/task regardless of shrink
aggressiveness; only K=3 (the most aggressive) stands out slightly, at 0.1069.
The small throughput cost looks like mostly a cost of *having shrinking active
at all*, rather than something that scales with *how often* a shrink fires.

**Practical takeaway**: shrinking carries a small, consistent throughput cost —
roughly 12–17% in relative terms, easy to miss in absolute terms — in exchange
for buffer savings ranging from ~51% to ~70% depending on K. For a scheduler
running many deques under a shared memory budget, that remains a good trade in
most cases, worth weighing against how tightly memory is actually constrained.

## 5. Epoch-based reclamation

The shrink logic swaps `curr` to a smaller, previously-allocated array on each
shrink. A naive version never actually `delete`s an old array once the owner has
grown past it — every array level stays allocated for the deque's entire
lifetime. That's safe (no thief can ever hold a dangling pointer) but means
buffer savings never translate into freed process memory.

Epoch-based reclamation closes this gap: each thief publishes its current global
epoch before touching the active array and clears it after; a retired buffer
(one the owner has shrunk away from) is only `delete`d once no thief's published
epoch predates the buffer's retirement — the same underlying idea as hazard
pointers or RCU, applied at the granularity of one epoch counter per thief slot
rather than per-pointer. This is the version benchmarked in §4.

### Validation: AddressSanitizer with an injected race window

To check the mechanism actually prevents a real use-after-free — rather than
just "not crashing," which (given x86's strong memory model) is not by itself
strong evidence of correctness — an artificial delay was inserted into
`steal()` between the moment a thief captures the active array pointer and the
moment it actually dereferences it, deliberately widening the exact race window
the epoch mechanism exists to protect against. The owner was driven through many
rapid grow/shrink cycles concurrently with this artificially slowed thief.

```
g++ -std=c++17 -O1 -g -fsanitize=address -DSIMULATE_SLOW_THIEF -pthread tester.cpp -o safe_asan
```

**Result: no use-after-free detected across multiple runs.** AddressSanitizer
did flag "leaked" allocations, but these are the expected, by-design permanently
retained grow-side arrays (present even without epoch reclamation) — not
evidence of a problem, and orthogonal to the use-after-free question this test
was checking.

### What this does and does not establish

A clean AddressSanitizer run under an artificially widened race window is
genuine, meaningful evidence that the epoch mechanism correctly holds a buffer
alive for as long as any thief might still be touching it. It does not, on its
own, prove the *original* never-free design was ever unsafe in the first place
— a thief's steal only succeeds if its `top` compare-and-swap confirms the
index it read was still live at that instant, and any correctly-implemented
`grow`/`copyinto` is obligated to preserve a still-live index's data on every
reactivation of a given array object, regardless of how many times that
physical buffer gets reused. The real, distinct risk epoch reclamation
addresses is different: once the design actually starts calling `delete`
(which the never-free version never did), a slow thief holding a stale pointer
into a freed buffer is a genuine use-after-free — precisely the scenario this
test was built to expose.

## 6. Memory ordering

The deque's synchronization state — `top`, `bottom`, and the active-array
pointer `curr` — was originally implemented using `std::atomic`'s default
memory order (`memory_order_seq_cst`) throughout. `seq_cst` is correct but the
most expensive ordering available: on weak-memory architectures it can require
a full hardware fence on every atomic operation, even ones that don't need a
global ordering guarantee. This work replaces the default with the weakest
ordering each specific operation actually needs, following the announce/check
(release/acquire) pattern.

| Operation | Order used | Reasoning |
|---|---|---|
| Owner reading its own last-written `bottom` | `relaxed` | Single-writer state — the owner always sees its own most recent write via plain program order |
| Owner's speculative `bottom` decrement/restore in `popBottom` | `relaxed` | Intermediate bookkeeping value, not relied on by any other thread before the race is resolved |
| Thief reading `top` / `bottom` | `acquire` | Needs to see the owner's published array writes and the current contested state |
| Owner reading `top` (to check against thief activity) | `acquire` | Needs to see the latest state after concurrent steals |
| Owner's `bottom.store()` after a push | `release` | A thief observing the new `bottom` value must also see the array write that happened before it |
| Every read of `curr` | `acquire` | Needed on **every** access — without it, a thief could see a new array pointer without the array's fully-constructed contents being visible yet (same failure mode as double-checked-locking-without-atomics) |
| Every write to `curr` (grow / shrink) | `release` | Publishes the newly constructed/populated array before the pointer swap becomes visible |
| Both contested `top.compare_exchange_strong` calls | `seq_cst`, kept explicit | `acq_rel` alone does not fully close the race between the owner's shrink logic and a thief's steal on genuinely weak-memory hardware (ARM, POWER) — the case the "Correct and Efficient Work-Stealing for Weak Memory Models" follow-up paper addresses |

A key correctness note on `curr`: unlike `top`/`bottom` (scalar counters),
`curr` is a pointer to an entire array's worth of data. `release`/`acquire` on
the pointer itself is required — a plain (non-atomic) pointer read/write is
undefined behavior in C++ even if it "happens to work" on x86 in practice. Even
keeping old arrays alive (§5's never-free case) doesn't fix this: that avoids
use-after-free, but not the separate problem of a thief observing a new pointer
value without the corresponding array contents being visible.

### Benchmark: seq_cst baseline vs. tuned ordering

Same shrink configuration (K=12, initial_log_size=4).

| Configuration | Avg throughput (us/task) | Avg array_size |
|---|---|---|
| All-`seq_cst` (default) | 0.1011 | 1414.2 |
| Tuned (`relaxed`/`acquire`/`release`, `seq_cst` retained only on contested CAS) | 0.0775 | 1316.5 |

**~23% throughput improvement** from ordering alone — memory footprint is
unaffected (shrink logic didn't change, only ordering did; the small
array_size difference between runs is normal sample variance).

## 7. ThreadSanitizer validation

```
g++ -std=c++17 -O1 -g -fsanitize=thread -pthread main.cpp -o deque_tsan
./deque_tsan
```

**Result: one race detected, and it is expected rather than a defect.**
`top`, `bottom`, and `curr` — all genuine synchronization state — are confirmed
race-free. The one flagged warning is a write in `pushBottom`
(`CircularArray::put`) racing a concurrent read in `steal`
(`CircularArray::get`) on the same array slot: a thief reads an array slot
speculatively, before the outcome is known, and only the subsequent `top`
compare-and-swap determines whether that read is valid or gets discarded. The
read is deliberately allowed to race the write — correctness comes from the
CAS validating (or invalidating) the read afterward, not from the read itself
being data-race-free.

This is a known, published gap in naively porting Chase-Lev's original
(Java-`volatile`-based) design to C++'s formal memory model, described in the
C++ standards committee's ["Tearable Atomics" proposal
(P0690)](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0690r1.html),
which uses this exact deque as its motivating example. Every real-world C++
Chase-Lev port carries this same property — it is not fixed by tightening
`top`/`bottom`/`curr` ordering, and is generally accepted as tolerable because
the CAS-based validation step makes it operationally safe even though it is
technically a data race under the strict standard.

## 8. Parallel merge sort vs. mutex-based queue

To demonstrate scalability in a realistic setting (not just a synthetic
push/steal microbenchmark), a parallel merge sort was built on top of the deque
and compared against a functionally identical implementation using a global
`std::queue` + `std::mutex` + `std::condition_variable` as the task pool.

### Design

- Array of `N = 1,000,000` random ints, sorted via task-based parallel merge sort.
- **Split phase**: recursively halve the range into `SPLIT` tasks down to
  `CUTOFF` elements, then sort sequentially.
- **Merge phase**: for ranges above `PARALLEL_MERGE_THRESHOLD` (4096), merging
  itself is parallelized using a CLRS-style parallel merge — repeatedly
  splitting the larger of the two runs at its midpoint, binary-searching the
  matching split point in the other run, and recursing on two independent
  halves.
- **Chase-Lev version**: one deque per worker thread; a worker pushes new
  subtasks onto its own deque and steals from others (round-robin victim
  selection) when its own deque is empty.
- **Mutex version**: identical task/merge logic, but all workers share one
  global queue guarded by a mutex, with `condition_variable` blocking (not
  busy-waiting) when no task is available — a genuinely competitive baseline.
- Both versions confirmed correct (`is_sorted` check) before any number was
  trusted.

A real bug surfaced and was fixed during development: an early version had
merge leaves copy their result back into the shared array immediately, which
raced against sibling merge tasks still reading from that same array for their
own split-point binary search — corrupting data mid-computation (manifesting as
both sort failures and, less predictably, hangs, since corrupted data fed into
split-point calculations produced a non-deterministic task tree even with a
fixed random seed). The fix: merge leaves write only to a scratch buffer; the
entire parallel-merge subtree performs one single, safe copy-back to the shared
array only once its root task completes.

### Result 1 — scaling with thread count (CUTOFF = 1024, 38,778 tasks)

| Threads | Mutex Queue Median | Chase-Lev Median | Speedup |
|---:|---:|---:|---:|
| 2 | 50.82 ms | 49.12 ms | 1.03× |
| 4 | 28.16 ms | 26.11 ms | 1.08× |
| 6 | 22.10 ms | 18.90 ms | 1.17× |
| 8 | 21.81 ms | 14.96 ms | 1.46× |
| 12 | 23.60 ms | 14.34 ms | 1.65× |

At low thread count the two are nearly tied. As thread count climbs, the
mutex's serialization cost compounds while Chase-Lev's lock-free steals barely
degrade, producing a widening, monotonic speedup — the textbook argument for
work-stealing, demonstrated empirically rather than asserted.

### Result 2 — cutoff sensitivity (8 threads fixed)

| Cutoff | Tasks Created | Chase-Lev Median | Mutex Queue Median | Speedup |
|---:|---:|---:|---:|---:|
| 256 | 44,922 | 16.90 ms | 24.24 ms | 1.43× |
| 512 | 40,826 | 15.21 ms | — | — |
| 1024 | 38,778 | **14.96 ms** | 21.81 ms | 1.46× |
| 2048 | 37,754 | 16.11 ms | 23.64 ms | 1.47× |
| 4096 | 37,242 | 15.20 ms | 20.86 ms | 1.37× |

Speedup peaks around cutoff 1024–2048 and tapers on both sides — too small a
cutoff means per-task bookkeeping overhead (atomic increments, deque
push/steal) starts to dominate real work; too large a cutoff sacrifices
parallelism granularity, compressing both curves toward each other. 1024 was
chosen as the working configuration for Result 1 based on this sweep, not an
arbitrary default.

## 9. SC fences (closing the IRIW gap)

After the deque's core operations were tuned with explicit
`acquire`/`release`/`relaxed` ordering (§6), two
`atomic_thread_fence(memory_order_seq_cst)` calls were added: one between
`steal`'s `top` read and `bottom` read, and one between `popBottom`'s `bottom`
write and its later `top` read.

### Why acquire/release alone isn't sufficient here

`acquire`/`release` only guarantees ordering between the two threads *directly*
synchronizing on one variable — it does not guarantee that a *third* thread
agrees on the relative order of operations on *different* variables. This is
the classic IRIW (Independent Reads of Independent Writes) anomaly: two threads
can each have a locally self-consistent, acquire/release-valid view of `top`
and `bottom`, while still disagreeing with each other about which happened
first — even though the `top` CAS itself is already `seq_cst` and correctly
arbitrates who wins a given index.

The fences close this gap by forcing the owner's `bottom`-then-`top` sequence
and a thief's `top`-then-`bottom` sequence to both participate in one single,
globally-agreed order — ruling out the scenario where the owner and a thief
simultaneously believe they've each legitimately claimed the same element. This
placement follows Lê, Pop, Cohen & Nardelli's "Correct and Efficient
Work-Stealing for Weak Memory Models," which identifies this exact gap in naive
acquire/release ports of Chase-Lev to C++11/C11.

### Why this wasn't (and likely couldn't be) caught by testing alone

Tens of thousands of runs on this project's development hardware (x86-64,
under WSL2) produced zero observable failures without the fences. This is
expected, not reassuring: x86-64's TSO (total store order) memory model is
strong enough to forbid most of the reorderings this gap depends on, as a
hardware guarantee independent of the C++ code's memory-order tags. A build
that's technically under-synchronized per the C++ standard can still be
"accidentally correct" on x86 for this reason. The anomaly is only reachable in
practice on genuinely weak-memory architectures (ARM, POWER) — which is
precisely why it took a formal paper, rather than testing, to surface it in the
first place. The fences were added to satisfy the C++ standard's actual memory
model guarantees, not to fix an observed failure.

## 10. Cache-line padding

To test whether false sharing between `top` and `bottom` was a meaningful
bottleneck, both fields were given `alignas(64)` cache-line padding, and the
merge sort benchmark (§8) was re-run across thread counts 1–12 (60 runs per
configuration, first 5 discarded as warm-up noise) with padding on and off,
back-to-back in the same session.

### Results (median time, microseconds)

| Threads | Unpadded | Padded | Difference |
|---|---|---|---|
| 1  | 62518 | 61433 | -1.74% |
| 2  | 31278 | 31726 | +1.43% |
| 4  | 18281 | 18319 | +0.21% |
| 6  | 13452 | 13459 | +0.05% |
| 8  | 11111 | 11008 | -0.93% |
| 12 | 9048  | 9083  | +0.39% |

### Conclusion

Padding produced **no statistically meaningful difference** at any thread count
tested — every delta is under 2%, well within normal run-to-run variance, and
the direction is inconsistent (padding is marginally faster at 1 and 8 threads,
marginally slower at 2/4/6/12). This suggests that for this workload, false
sharing on `top`/`bottom` was not the dominant contention point; the bottleneck
is more likely elsewhere in the work-stealing/merge logic. That said, as with
the other benchmarks in this project, this was measured on WSL2 — its
virtualization/scheduling overhead could plausibly be masking a real but small
false-sharing effect that would show up more clearly on bare metal. The padding
was kept in the implementation as a documented, tested consideration, but is
not claimed as a performance win.

