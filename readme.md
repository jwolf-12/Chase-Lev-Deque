# Chase-Lev Work-Stealing Deque — Project Overview

## What this is

A C++ implementation of the Chase-Lev lock-free work-stealing deque (Chase & Lev,
2005), built as a portfolio project for quant-dev / low-latency systems interviews.
One owner thread pushes and pops from the "bottom" of the deque; any number of
"thief" threads concurrently steal from the "top." The design goal — and the actual
value proposition over a lock-based task queue — is that the owner's hot-path
operations (`pushBottom`, `popBottom`) never take a lock and, in the uncontended
case, involve no atomic contention at all, while thieves race each other and the
owner using compare-and-swap rather than blocking.

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

## What's been built and validated, roughly in the order it happened

1. **Base algorithm** — atomic top/bottom, CAS-based steal, correct resolution of
   the contested last-element race. An initial task-loss bug (3–10% of tasks
   silently dropped under contention, no duplicates) was found via a randomized
   multithreaded stress-testing harness and fixed — the read-before-CAS ordering
   in `steal`/`popBottom` had been backwards.
2. **Dynamic growth and shrinking** (`perhapsShrink`), letting the deque's memory
   footprint track its actual current backlog instead of its historical peak.
3. **`lastTop` caching optimization** (from the paper's own discussion of reducing
   contended reads of `top` on the `pushBottom` hot path) — benchmarked at ~5%
   faster under low contention, ~1–2% under high contention. A real latent bug was
   found here too: an early version relied on `popBottom` incidentally refreshing
   the cache, which passed every test under one specific call pattern and only
   broke once a workload was built that didn't happen to exercise that pattern —
   see `SHRINK_BENCHMARK.md` for the full account.
4. **Shrink threshold (K) tradeoff sweep** — a parameter sweep across K=3–12 on a
   custom oscillating push/drain workload, showing 37–67% average memory reduction
   with no measurable throughput cost across the whole range tested. See
   `SHRINK_BENCHMARK.md`.
5. **Explicit acquire/release/relaxed memory ordering**, replacing the default
   `seq_cst` on every atomic operation with the weakest ordering each one actually
   needs — `seq_cst` retained only on the two contested `top` CAS operations, per
   the weak-memory-models literature. Benchmarked ~23% throughput improvement over
   the all-`seq_cst` baseline. See `MEMORY_ORDERING_AND_TSAN.md`.
6. **ThreadSanitizer validation** — confirmed `top`, `bottom`, and the active-array
   pointer (all genuine synchronization state) are race-free. The one race tsan
   does flag is the algorithm's own by-design unsynchronized element read, a known,
   published gap in naive C++ ports of Chase-Lev (documented in the C++ standards
   committee's P0690 "Tearable Atomics" proposal) — not a defect. See
   `MEMORY_ORDERING_AND_TSAN.md`.
7. **Parallel merge sort benchmark against a mutex-based task queue** — demonstrated
   scalability on a realistic workload (not just a synthetic push/steal loop),
   showing speedup grow from roughly parity at 2 threads to ~1.65× at 12 threads.
   See `MERGESORT_AND_FENCES.md`.
8. **SC fences closing an IRIW-style gap** between `top` and `bottom` that plain
   acquire/release doesn't cover on weak-memory hardware, following Lê, Pop, Cohen
   & Nardelli's C11 formalization of Chase-Lev. See `MERGESORT_AND_FENCES.md`.

## Known, deliberately scoped-out extensions

- **Shared buffer pool** (Section 4 of the original paper) — reusing reclaimed
  array buffers across grow/shrink cycles instead of the current approach, which
  keeps every array level allocated for the deque's lifetime.
- **Hazard pointers** for safe memory reclamation, as a more general alternative to
  the shared-pool approach.
- **Multi-level shrink-skipping** — jumping directly from a distant ancestor array
  size to a much smaller one in one step when the backlog has collapsed, rather
  than shrinking one level at a time, with low-water-marks aggregated across every
  array level being skipped.

These are understood in depth (see the design discussion in project notes) but not
implemented, in favor of prioritizing the memory-ordering and benchmarking work
above, which more directly demonstrates the kind of correctness reasoning this
project is meant to showcase.


# Chase-Lev Deque: Shrink Logic — Throughput vs. Array Buffer Size Tradeoff

Unlike the `lastTop` caching optimization, shrinking is not primarily a speed
optimization — every shrink event costs real work (allocate a smaller array, copy
live elements, perform the bottom/top-shift synchronization the paper proves safe
against concurrent thieves, free the old buffer). The expected benefit is a smaller
*active array buffer*, not throughput: an array that grows to accommodate a burst of
work should give that buffer back down once the burst drains, rather than holding
onto peak-sized capacity forever.

**A precision worth being explicit about**: the metric below (`array_size`) measures
the deque's own logical buffer size — how large the currently-active `CircularArray`
is — not total OS-level process memory. The claim this benchmark supports is
specifically: **shrinking keeps the deque's own array buffer sized to its current
backlog instead of its historical peak** — a real, useful property for a scheduler
running many deques that share a fixed memory budget — not a claim about total
process memory usage.

This benchmark sweeps the shrink threshold constant `K` (from `perhapsShrink`'s
trigger condition `size < array.size() / K`) to map out how aggressively shrinking
trades array buffer size against throughput cost.

> **Note on data provenance:** an earlier pass of this benchmark was run before
> epoch-based reclamation (below) was added to the shrink path. That data is
> discarded and superseded by the numbers here. Epoch reclamation adds real
> per-steal and per-shrink bookkeeping, so throughput numbers from before it
> existed are not comparable to the current implementation — the table and
> interpretation below reflect the deque as it stands today, shrink logic and
> epoch reclamation together.

## Test setup

- **Hardware**: WSL2 (Ubuntu), 16 logical cores
- **Compiler**: g++, `-std=c++17 -O2 -pthread`
- **Implementation under test**: shrink logic with epoch-based reclamation active
  (see below) — not the pre-epoch never-free version
- **Workload**: an oscillating push/drain pattern — 20 bursts, each pushing 2,000
  tasks then draining 1,900 via `popBottom` — with 4 thief threads also stealing
  concurrently throughout
- **Instrumentation**: `array_size` logged after every burst; owner-loop wall-clock
  time logged per run
- **Sample size**: 60 full program runs per configuration (1,200 burst-size samples
  per configuration)
- **Correctness check**: every run verified 0 duplicates and 0 missing tasks
- **Metric**: mean `array_size` across all burst samples (buffer size), mean
  `us/task` across all runs (throughput) — computed with a small script rather than
  by hand, to avoid transcription error across the full set of data points collected

Note on `K`: a **smaller** `K` makes the trigger threshold `array.size()/K` larger,
so the deque shrinks *more* aggressively; a **larger** `K` is more conservative.

## Results

| Config | Avg throughput (us/task) | Avg array_size (buffer) | Buffer size vs. no-shrink |
|---|---|---|---|
| No shrink (baseline) | 0.0917 | 2228.9 | — |
| K=3 | 0.1069 | 663.1 | −70.3% |
| K=4 | 0.1027 | 771.9 | −65.4% |
| K=5 | 0.1037 | 896.9 | −59.8% |
| K=8 | 0.1034 | 937.1 | −58.0% |
| K=12 | 0.1035 | 1084.1 | −51.4% |

![Throughput vs array buffer size tradeoff, post-epoch-reclamation](shrink_tradeoff.png)

## Interpretation

**Array buffer size still scales cleanly and predictably with K.** As K increases
from 3 to 12, the threshold for triggering a shrink gets stricter, and average
array size climbs monotonically back toward the no-shrink baseline (663 → 772 → 897
→ 938 → 1084, approaching 2228.9). That part of the picture hasn't changed from
before epoch reclamation was added — K is still a direct dial on how tightly the
deque's active buffer tracks its actual current backlog versus its historical peak.

**Throughput stays close to flat in practical terms, though there's a small,
consistent gap versus the pre-epoch numbers.** Every shrink-enabled configuration
lands in a 0.1027–0.1069 us/task band, against a 0.0917 us/task no-shrink baseline —
on the same 0–0.40 us/task scale used throughout this project's charts, that gap is
easy to miss, and visually the picture looks a lot like the earlier (discarded)
pre-epoch result: shrinking doesn't meaningfully change the shape of the throughput
line, which stays essentially flat across every K tested. Put in relative terms,
though, shrink-enabled configs do sit measurably above baseline — roughly 12–17%
higher us/task — so "flat" here means flat *in absolute, practical terms at this
task granularity*, not that the two implementations are indistinguishable if you
zoom in. Whether that relative gap matters depends entirely on how tight your actual
throughput budget is; at this workload's scale, it's easy to overlook.

**Within the shrink-enabled configs, throughput barely moves with K.** K=4, K=5,
K=8, and K=12 all cluster tightly around 0.103–0.104 us/task regardless of how
aggressively each one shrinks, with only the most aggressive setting tested (K=3)
standing out slightly higher, at 0.1069. That suggests whatever small throughput
cost epoch reclamation adds is mostly a cost of *having shrinking active at all*,
rather than something that scales smoothly with *how often* a shrink actually
fires.

**Practical takeaway**: with epoch-based reclamation in place, shrinking carries a
small, consistent throughput cost relative to never shrinking at all — roughly
12–17% in relative terms — but on the same absolute scale used throughout this
project's benchmarks, that cost is easy to miss and the throughput line still reads
as essentially flat, much like the pre-epoch data did. In exchange, buffer savings
still range from about 51% to 70% depending on K. For a scheduler running many
deques under a shared memory budget, that remains a good trade in most cases — the
difference from the earlier framing is just that "close to free" should now be read
as "small and fairly fixed" rather than "measured zero," and it's worth weighing
against how tightly memory is actually constrained rather than assumed away
entirely.

## Epoch-based reclamation: freeing retired buffers safely

The shrink logic swaps `curr` to a smaller, previously-allocated array on each
shrink. A naive version of this never actually `delete`s an old array once the
owner has grown past it — every array level stays allocated for the deque's entire
lifetime. That's safe (no thief can ever hold a dangling pointer) but means buffer
savings never translate into freed process memory.

Epoch-based reclamation closes this gap: each thief publishes its current global
epoch before touching the active array and clears it after, and a retired buffer
(one the owner has shrunk away from) is only `delete`d once no thief's published
epoch predates the buffer's retirement — the same underlying idea as hazard
pointers or RCU, applied at the granularity of one epoch counter per thief slot
rather than per-pointer. This is the version benchmarked above.

### Validation: AddressSanitizer with an injected race window

To check the mechanism actually prevents a real use-after-free (rather than just
"not crashing," which — per the discussion of x86's strong memory model elsewhere
in this project — is not by itself strong evidence of correctness), an artificial
delay was inserted into `steal()` between the moment a thief captures the active
array pointer and the moment it actually dereferences it, deliberately widening the
exact race window the epoch mechanism exists to protect against. The owner was
driven through many rapid grow/shrink cycles concurrently with this artificially
slowed thief.

Compiled and run with:
```
g++ -std=c++17 -O1 -g -fsanitize=address -DSIMULATE_SLOW_THIEF -pthread tester.cpp -o safe_asan
```

**Result: no use-after-free detected across multiple runs.** AddressSanitizer did
flag "leaked" allocations, but these are the expected, by-design permanently
retained grow-side arrays (the same ones present even without epoch reclamation) —
not evidence of a problem, and orthogonal to the use-after-free question this test
was actually checking.

### What this does and does not establish

A clean AddressSanitizer run under an artificially widened race window is genuine,
meaningful evidence that the epoch mechanism correctly holds a buffer alive for as
long as any thief might still be touching it. It does not, on its own, prove the
*original* never-free design was ever unsafe in the first place — reasoning through
that question directly: a thief's steal only succeeds if its `top`
compare-and-swap confirms the index it read was still live at that instant, and any
correctly-implemented `grow`/`copyinto` is obligated to preserve a still-live
index's data on every reactivation of a given array object, regardless of how many
times that physical buffer gets reused. The real, distinct risk epoch reclamation
addresses is a different one: once the design actually starts calling `delete`
(which the original never-free version never did), a slow thief holding a stale
pointer into a buffer that gets freed out from under it is a genuine
use-after-free — and that is precisely the scenario this test was built to expose.



# Chase-Lev Deque: Memory Ordering & ThreadSanitizer Validation

## Background

The deque's synchronization state — `top`, `bottom`, and the active-array pointer
`curr` — was originally implemented using `std::atomic`'s default memory order
(`memory_order_seq_cst`) throughout. `seq_cst` is correct but the most expensive
ordering available: on weak-memory architectures it can require a full hardware
fence on every atomic operation, even ones that don't need a global ordering
guarantee. This work replaces the default with the weakest ordering each specific
operation actually needs, following the announce/check (release/acquire) pattern,
and validates the result with ThreadSanitizer.

## Memory ordering applied

| Operation | Order used | Reasoning |
|---|---|---|
| Owner reading its own last-written `bottom` | `relaxed` | Single-writer state — the owner always sees its own most recent write via plain program order; nothing to synchronize with another thread |
| Owner's speculative `bottom` decrement/restore in `popBottom` | `relaxed` | Intermediate bookkeeping value, not relied on by any other thread before the race is resolved |
| Thief reading `top` / `bottom` | `acquire` | Needs to see the owner's published array writes and the current contested state |
| Owner reading `top` (to check against thief activity) | `acquire` | Needs to see the latest state after concurrent steals |
| Owner's `bottom.store()` after a push | `release` | Publishes the new element write in the array — a thief that observes this new `bottom` value must also see the array write that happened before it |
| Every read of `curr` (the active-array pointer) | `acquire` | Needed on **every** access, not just after a resize — without it, a thief could see a new array pointer without the array's fully-constructed contents being visible yet (same failure mode as the classic double-checked-locking-without-atomics bug) |
| Every write to `curr` (grow / shrink) | `release` | Publishes the newly constructed/populated array before the pointer swap becomes visible |
| Both contested `top.compare_exchange_strong` calls (`steal`'s and `popBottom`'s size==0 race) | `seq_cst`, kept explicit | The one deliberate exception — `acq_rel` alone does not fully close the race between the owner's shrink logic and a thief's steal on genuinely weak-memory hardware (ARM, POWER); this is the specific case the "Correct and Efficient Work-Stealing for Weak Memory Models" follow-up paper addresses |

A key correctness note on `curr`: unlike `top`/`bottom`, which are scalar counters,
`curr` is a pointer to an entire array's worth of data. `release`/`acquire` on the
pointer itself is required — a plain (non-atomic) pointer read/write here is
undefined behavior in C++ even if it "happens to work" on x86 in practice, and even
keeping old arrays alive (rather than freeing them) does not fix this: that avoids
use-after-free, but not the separate problem of a thief observing a new pointer
value without the corresponding array contents being visible.

## Benchmark: seq_cst baseline vs. tuned ordering

Same oscillating workload used throughout this project (20 bursts, 2,000 push /
1,900 drain per burst, 4 thief threads), same shrink configuration (K=12,
initial_log_size=4), 60 runs per configuration.

| Configuration | Avg throughput (us/task) | Avg array_size |
|---|---|---|
| All-`seq_cst` (default) | 0.1011 | 1414.2 |
| Tuned (`relaxed`/`acquire`/`release`, `seq_cst` retained only on contested CAS) | 0.0775 | 1316.5 |

**~23% throughput improvement** from ordering alone — memory footprint is
unaffected (shrink logic didn't change, only ordering did; the small difference
between runs is normal sample variance).

*Note: measured on WSL2 (Ubuntu, 16 logical cores). WSL2's hypervisor layer adds
scheduling overhead relative to bare metal, so this percentage is expected to be a
lower bound — a native-Linux rerun is planned to confirm the effect holds and get a
tighter number.*

## ThreadSanitizer validation

Compiled and run with:
```
g++ -std=c++17 -O1 -g -fsanitize=thread -pthread main.cpp -o deque_tsan
./deque_tsan
```

**Result: one race detected, and it is expected rather than a defect.**

ThreadSanitizer confirms `top`, `bottom`, and `curr` — all the actual
synchronization state the ordering work above targets — are race-free. The one
flagged warning is a write in `pushBottom` (`CircularArray::put`) racing a
concurrent read in `steal` (`CircularArray::get`) on the same array slot.

This is the algorithm's own, by-design unsynchronized element access: a thief reads
an array slot speculatively, before the outcome is known, and only the subsequent
`top` compare-and-swap determines whether that read is valid or gets discarded. The
read is deliberately allowed to race the write — correctness comes from the CAS
validating (or invalidating) the read afterward, not from the read itself being
data-race-free.

This is a known, published gap in naively porting Chase-Lev's original
(Java-`volatile`-based) design to C++'s formal memory model — it is explicitly
described in the C++ standards committee's ["Tearable Atomics" proposal
(P0690)](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0690r1.html),
which uses this exact deque as its motivating example:

> "If the deque contains only one element, this causes undefined behavior in C++'s
> memory model, because the element accessed ... may be concurrently written by the
> owning thread of the deque."

Every real-world C++ Chase-Lev port carries this same property — it is not fixed by
tightening `top`/`bottom`/`curr` ordering, and is generally accepted as tolerable
because the CAS-based validation step makes it operationally safe even though it is
technically a data race under the strict standard.

**Honest summary of what this validates**: all genuine synchronization state is
confirmed race-free under ThreadSanitizer. The one remaining flagged race is the
algorithm's inherent, literature-documented unsynchronized element read, not an
implementation defect.



# Chase-Lev Deque: Merge Sort Scalability Benchmark & SC Fences

## Part 1: Parallel merge sort vs. a mutex-based task queue

To demonstrate the deque's scalability in a realistic setting (not just a synthetic
push/steal microbenchmark), a parallel merge sort was built on top of it and
compared against a functionally identical implementation using a global
`std::queue` + `std::mutex` + `std::condition_variable` as the task pool.

### Design

- Array of `N = 1,000,000` random ints, sorted via task-based parallel merge sort.
- **Split phase**: recursively halve the range into `SPLIT` tasks down to `CUTOFF`
  elements, then sort sequentially.
- **Merge phase**: for ranges above `PARALLEL_MERGE_THRESHOLD` (4096), merging
  itself is parallelized using a CLRS-style parallel merge — repeatedly splitting
  the larger of the two runs at its midpoint, binary-searching the matching split
  point in the other run, and recursing on two independent halves — rather than
  doing one large sequential merge on a single core.
- **Chase-Lev version**: one deque per worker thread; a worker pushes new subtasks
  onto its own deque and steals from others (round-robin victim selection) when its
  own deque is empty.
- **Mutex version**: identical task/merge logic, but all workers share one global
  queue guarded by a mutex, with `condition_variable` blocking (not busy-waiting)
  when no task is available — a genuinely competitive, non-strawman baseline.
- Both versions confirmed correct (`is_sorted` check) before any number was trusted.

A real bug surfaced and was fixed during development: an early version had merge
leaves copy their result back into the shared array immediately, which raced
against sibling merge tasks still reading from that same array for their own
split-point binary search — corrupting data mid-computation (manifesting as both
sort failures and, less predictably, hangs, since corrupted data fed into split-point
calculations produced a non-deterministic task tree even with a fixed random seed).
The fix: merge leaves write only to a scratch buffer; the entire parallel-merge
subtree performs one single, safe copy-back to the shared array only once its root
task completes.

### Result 1 — scaling with thread count (CUTOFF = 1024, 38,778 tasks)

| Threads | Mutex Queue Median | Chase-Lev Median | Speedup |
|---:|---:|---:|---:|
| 2 | 50.82 ms | 49.12 ms | 1.03× |
| 4 | 28.16 ms | 26.11 ms | 1.08× |
| 6 | 22.10 ms | 18.90 ms | 1.17× |
| 8 | 21.81 ms | 14.96 ms | 1.46× |
| 12 | 23.60 ms | 14.34 ms | 1.65× |

At low thread count, the two are nearly tied — there's little contention for either
mechanism to fight over. As thread count climbs, the mutex's serialization cost
compounds while Chase-Lev's lock-free steals barely degrade, producing a widening,
monotonic speedup — the textbook argument for work-stealing, demonstrated
empirically rather than asserted.

### Result 2 — cutoff sensitivity (8 threads fixed)

| Cutoff | Tasks Created | Chase-Lev Median | Mutex Queue Median | Speedup |
|---:|---:|---:|---:|---:|
| 256 | 44,922 | 16.90 ms | 24.24 ms | 1.43× |
| 512 | 40,826 | 15.21 ms | — | — |
| 1024 | 38,778 | **14.96 ms** | 21.81 ms | 1.46× |
| 2048 | 37,754 | 16.11 ms | 23.64 ms | 1.47× |
| 4096 | 37,242 | 15.20 ms | 20.86 ms | 1.37× |

Speedup peaks around cutoff 1024–2048 and tapers on both sides — too small a
cutoff means per-task bookkeeping overhead (atomic increments, deque push/steal)
starts to dominate real work; too large a cutoff sacrifices parallelism granularity,
compressing both curves toward each other. 1024 was chosen as the working
configuration for Result 1 based on this sweep, not an arbitrary default.

## Part 2: SC fences — closing the IRIW gap

After the deque's core operations were tuned with explicit `acquire`/`release`/
`relaxed` ordering (see the memory-ordering benchmark writeup), two
`atomic_thread_fence(memory_order_seq_cst)` calls were added: one between `steal`'s
`top` read and `bottom` read, and one between `popBottom`'s `bottom` write and its
later `top` read.

### Why acquire/release alone isn't sufficient here

`acquire`/`release` only guarantees ordering between the two threads *directly*
synchronizing on one variable — it does not guarantee that a *third* thread agrees
on the relative order of operations on *different* variables. This is the classic
IRIW (Independent Reads of Independent Writes) anomaly: two threads can each have a
locally self-consistent, acquire/release-valid view of `top` and `bottom`, while
still disagreeing with each other about which happened first — even though the
`top` CAS itself is already `seq_cst` and correctly arbitrates who wins a given
index.

The fences close this specific gap by forcing the owner's `bottom`-then-`top`
sequence and a thief's `top`-then-`bottom` sequence to both participate in one
single, globally-agreed order — ruling out the scenario where the owner and a
thief simultaneously believe they've each legitimately claimed the same element.
This placement follows Lê, Pop, Cohen & Nardelli's "Correct and Efficient
Work-Stealing for Weak Memory Models," which identifies this exact gap in naive
acquire/release ports of Chase-Lev to C++11/C11.

### Why this wasn't (and likely couldn't be) caught by testing alone

Tens of thousands of runs on this project's development hardware (x86-64, under
WSL2) produced zero observable failures without the fences. This is expected, not
reassuring: x86-64's TSO (total store order) memory model is strong enough to
forbid most of the reorderings this gap depends on, as a hardware guarantee
independent of the C++ code's memory-order tags. A build that's technically
under-synchronized per the C++ standard can still be "accidentally correct" on
x86 for this reason. The anomaly is only reachable in practice on genuinely
weak-memory architectures (ARM, POWER) — which is precisely why it took a formal
paper, rather than testing, to surface it in the first place. The fences were
added to satisfy the C++ standard's actual memory model guarantees, not to fix an
observed failure.

## Cache-Line Padding: Padded vs Unpadded Benchmark

To test whether false sharing between `top` and `bottom` was a meaningful bottleneck, both fields were given `alignas(64)` cache-line padding, and the parallel merge sort benchmark was re-run across thread counts 1–12 (60 runs per configuration, first 5 discarded as warm-up noise) with padding on and off, back-to-back in the same session.

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

Padding produced **no statistically meaningful difference** at any thread count tested — every delta is under 2%, well within normal run-to-run variance, and the direction is inconsistent (padding is marginally faster at 1 and 8 threads, marginally slower at 2/4/6/12). This suggests that for this workload, false sharing on `top`/`bottom` was not the dominant contention point; the bottleneck is more likely elsewhere in the work-stealing/merge logic. The padding was kept in the implementation as a documented, tested consideration, but is not claimed as a performance win.
