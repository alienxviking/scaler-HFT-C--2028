# Order Book Capstone - Report

Snehangshu Roy (24BCS10155), Saniya Patil (24BCS10246)

## 1. The three implementations

All three books store prices as integer ticks (`100.01 -> 10001`). A level's qty is
the sum of the qtys of its orders, and since every order has qty >= 1, a level
with qty 0 has no orders, so none of the books keep a separate order count per level.
None of them keep the individual orders inside a level either: there is no matching, so
nobody ever needs FIFO order inside a level, only the total.

### A - `std::map` + `std::unordered_map`

- **Orders**: `std::unordered_map<uint64_t, Order>` where `Order = {side, tick, qty}`.
  One heap node per order.
- **Levels**: one `std::map<int, long>` per side (red-black tree, one node per level).
  Bids use `std::greater<int>` so `begin()` is the best price on both sides.
- **Best bid/ask**: `begin()`, which `std::map` keeps cached, so O(1).
- **Cancel**: `unordered_map::find(id)`. libstdc++ uses chained hashing, so this is the
  bucket array -> pointer to the node that holds side, tick, qty. Then `map.find(tick)`,
  subtract, erase the level node if it hits 0. (C also hashes the id, but with open
  addressing in one flat array. Section 11.3 shows how much that difference matters.)

| op | complexity |
|---|---|
| add_order | O(1) avg hash insert + O(log P) map lookup/insert = **O(log P)** |
| cancel_order | O(1) avg hash find + O(log P) map find = **O(log P)** |
| get_bbo | **O(1)** |
| get_bids / get_asks | **O(d)** (walk the map from begin) |

### B - `std::priority_queue` with lazy deletion + `std::map` for orders

- **Orders**: `std::map<uint64_t, Order>`, an ordered tree keyed by id. We picked a tree
  here on purpose so cancel lookup is different from A and C (O(log N) instead of a hash).
- **Levels**: `std::unordered_map<int, LevelInfo>` per side, `LevelInfo = {qty, in_heap}`.
- **Best bid/ask**: max-heap of ticks for bids, min-heap for asks. A heap can't delete
  from the middle, so when a level empties we leave its tick in the heap ("stale") and only
  remove it once it gets to the top. Every cancel that empties the top level pops
  stale ticks until the top is live again, so `get_bbo()` just reads `top()`.
  The `in_heap` flag makes sure a tick is pushed at most once, so the heap never has more
  than 5000 entries per side even after millions of ops.
- **Cancel**: tree lookup of the id, subtract from the level in the hash map, clean the heap
  top if needed.
- **Depth**: the heap isn't sorted, so `get_bids(d)` copies the heap and pops until it has
  d live levels.

| op | complexity |
|---|---|
| add_order | O(log N) tree insert + O(1) hash + O(log P) heap push only for a new level = **O(log N)** |
| cancel_order | O(log N) tree find/erase + **amortized O(log P)** for popping stale tops (each tick is popped once per push) |
| get_bbo | **O(1)** (top + one hash lookup) |
| get_bids / get_asks | **O(P + (d + s) log P)**: copy the heap, then pop until d live levels, s = stale ticks popped on the way, so worst case O(P log P) |

### C - our design: flat price array + bitmap + own hash table

- **Levels**: one `long qty_[10001]` array indexed by `tick - 5000`. The whole price range
  is only 10,001 ticks, so we just allocate all of it (80 KB). Index 5000 (100.00) is never used,
  which also separates bids and asks.
- **Best bid/ask**: cached `best_bid_` / `best_ask_` index. Updating on add is one compare.
  On cancel, if the best level becomes empty we need the next one, so we keep a bitmap
  `used_[157]` (1 bit per level, ~1.2 KB) and scan it 64 levels per step using
  `__builtin_clzll` / `__builtin_ctzll`. Worst case is 79 words for a side.
- **Orders**: our own open addressing hash table `OrderTable`: one `std::vector` of
  16 byte slots `{id, tick, qty}`, power of 2 size, fibonacci hashing, linear probing,
  max load 0.5 (it doubles before it gets more than half full). `tick == 0` marks an empty slot. Deletion uses
  backward shift (move later entries of the cluster back into the hole), so there are no
  tombstones and lookups never walk over dead slots.
- **Cancel**: hash lookup gives tick and qty directly (no pointer to follow), subtract from
  `qty_[i]`, clear the bit and rescan only if the level emptied and it was the best one.

| op | complexity |
|---|---|
| add_order | **O(1) amortized** (O(N) on the rare resize) |
| cancel_order | **O(1) expected**, O(P/64) worst case when the best level empties |
| get_bbo | **O(1)** |
| get_bids / get_asks | **O(d + P/64)** worst case |

## 2. Setup

- AMD Ryzen 9 8940HX (16 cores / 32 threads, 1 MB L2 per core, 32 MB L3), laptop
- Windows 11, Docker Desktop -> WSL2 kernel 6.6.87, Ubuntu 24.04 container
- g++ 13.3.0, `-std=c++17 -O2 -g -fno-omit-frame-pointer`, perf 6.8.12
- every counter from the assignment was available. There are more events than hardware
  counters, so perf multiplexes them: each one counted ~85% of the time and perf scales it up
  (the last column in the perf stat output)

It's a laptop inside a VM (and a small local kind cluster from another course was idling in
the background, ~0.2 of a core), so there's noise. Within one session the `+-` in perf stat is
a few %, but between sessions the medians moved more than that for the slow ops. For example,
B's cancel was between 312 and 421 ns in different sessions. The ranking never changed. All
numbers below come from the same final run of `run_perf.sh`.

## 3. Results table

Benchmark numbers are medians of 5 runs from `./bench --impl X` (`results/bench_X.txt`).
perf numbers are from `perf stat -r 5 ... ./bench --impl X` exactly as in the assignment
(`results/perf_stat_X.txt`).

| Metric | Implementation A | Implementation B | Implementation C |
|---|---|---|---|
| Total time (ms) | 124.9 | 138.2 | **30.7** |
| ns / add | 115.5 | 72.8 | **47.5** |
| ns / cancel | 234.2 | 421.3 | **29.5** |
| ns / BBO | 1.69 | 3.07 | **1.52** |
| IPC | 0.79 | 1.02 | **1.15** |
| Branch misses (%) | 5.83% (93.4M) | 2.67% (60.9M) | 3.17% (21.1M) |
| L1-dcache-load-misses | 196.7M | 317.0M | **45.4M** |
| Cache misses | 61.8M | 90.8M | **14.2M** |
| Page faults | 19,610 | **18,787** | 25,152 |
| dTLB-load-misses | 7.19M | 12.0M | **5.44M** |

The add and cancel numbers have the ~18 ns `steady_clock::now()` overhead subtracted (the raw
numbers are in `results/bench_X.txt`). One `get_bbo` call is shorter than one clock read,
so it's timed in batches of 16 back to back calls (see README). The BBO numbers above are per call.

Per-op numbers and total time measure different things. Timing each op on its own (with a
clock read on each side) stops the CPU from overlapping one op's cache misses with the next
op's work, so per-op is closer to latency and total / ops is throughput. For C, the weighted
per-op average is 0.7 x 47.5 + 0.2 x 29.5 + 0.1 x 1.5 = 39 ns, more than the 30.7 ns per op
of the untimed loop.

### Cleaner counters

When we looked at `perf record` we saw `__vdso_clock_gettime` taking 11% of A's samples, 10%
of B's and **29% of C's** (37% together with `steady_clock::now` itself). The per-op timing pass
was polluting the counters. It also pulls IPC down: reading the clock waits for the
instructions before it to finish, so C's IPC is 1.26 for the book alone (table below) but only
1.15 in the full run. The perf stat for the whole program also includes generating the 1M ops
and the final replay, which is the same work for all three. So we did two more things:

1. `--no-timers` runs only the untimed pass (`results/perf_stat_X_no_timers.txt`)
2. `per_run_counters.sh` runs `perf stat` with `--runs 1` and `--runs 5` (both `--no-timers`)
   and computes `(runs5 - runs1) / 4`. That's the cost of **one pass of 1M ops on one book**,
   including creating and destroying the book, with all the shared stuff cancelled out
   (`results/perf_per_run.txt`):

| per run (1M ops) | A | B | C |
|---|---|---|---|
| cycles | 730M | 749M | **157M** |
| instructions | 545M | 787M | **198M** |
| IPC | 0.75 | 1.05 | **1.26** |
| branches | 119.6M | 175.6M | **32.6M** |
| branch-misses | 7.72M (6.5%) | 4.86M (2.8%) | **0.99M (3.0%)** |
| cache-misses | 5.32M | 8.13M | **1.22M** |
| L1-dcache-load-misses | 17.2M | 28.1M | **3.6M** |
| dTLB-load-misses | 0.60M | 1.05M | **0.43M** |
| page-faults | 0 | 0 | 0 |

We use this table for most of the analysis below.

### perf stat output

`perf stat -r 5 -e task-clock,cycles,instructions,branches,branch-misses,cache-misses,L1-dcache-load-misses,dTLB-load-misses,page-faults ./bench --impl X`

**A**
```
 Performance counter stats for './bench --impl A' (5 runs):

           2013.92 msec task-clock                       #    0.999 CPUs utilized               ( +-  1.47% )
        9715418349      cycles                           #    4.824 GHz                         ( +-  0.85% )  (85.70%)
        7705702756      instructions                     #    0.79  insn per cycle              ( +-  0.19% )  (85.73%)
        1600826779      branches                         #  794.880 M/sec                       ( +-  0.22% )  (85.71%)
          93390315      branch-misses                    #    5.83% of all branches             ( +-  0.20% )  (85.70%)
          61775696      cache-misses                                                            ( +-  1.10% )  (85.73%)
         196744365      L1-dcache-load-misses                                                   ( +-  0.11% )  (85.70%)
           7191714      dTLB-load-misses                                                        ( +-  0.69% )  (85.73%)
             19610      page-faults                      #    9.737 K/sec                       ( +-  0.00% )

            2.0168 +- 0.0297 seconds time elapsed  ( +-  1.47% )
```

**B**
```
 Performance counter stats for './bench --impl B' (5 runs):

           2155.89 msec task-clock                       #    0.999 CPUs utilized               ( +-  1.83% )
       10498391670      cycles                           #    4.870 GHz                         ( +-  1.42% )  (85.67%)
       10706002767      instructions                     #    1.02  insn per cycle              ( +-  0.07% )  (85.67%)
        2285522927      branches                         #    1.060 G/sec                       ( +-  0.09% )  (85.68%)
          60943988      branch-misses                    #    2.67% of all branches             ( +-  0.34% )  (85.68%)
          90754537      cache-misses                                                            ( +-  0.60% )  (85.78%)
         317045768      L1-dcache-load-misses                                                   ( +-  0.13% )  (85.86%)
          11991866      dTLB-load-misses                                                        ( +-  0.92% )  (85.67%)
             18787      page-faults                      #    8.714 K/sec                       ( +-  0.00% )

            2.1587 +- 0.0394 seconds time elapsed  ( +-  1.83% )
```

**C**
```
 Performance counter stats for './bench --impl C' (5 runs):

            728.54 msec task-clock                       #    0.996 CPUs utilized               ( +-  2.59% )
        3536309501      cycles                           #    4.854 GHz                         ( +-  2.28% )  (85.48%)
        4052859079      instructions                     #    1.15  insn per cycle              ( +-  0.44% )  (85.45%)
         665458335      branches                         #  913.419 M/sec                       ( +-  0.34% )  (85.44%)
          21072627      branch-misses                    #    3.17% of all branches             ( +-  1.32% )  (85.81%)
          14248841      cache-misses                                                            ( +-  0.83% )  (85.97%)
          45374170      L1-dcache-load-misses                                                   ( +-  3.25% )  (85.97%)
           5435587      dTLB-load-misses                                                        ( +-  2.76% )  (85.88%)
             25152      page-faults                      #   34.524 K/sec                       ( +-  0.00% )

            0.7312 +- 0.0189 seconds time elapsed  ( +-  2.59% )
```

### perf record (top of `perf report --no-children`, full output in `results/perf_report_X.txt`)

| A | | B | | C | |
|---|---|---|---|---|---|
| `MapBook::add_order` | 32.4% | `HeapBook::cancel_order` | 24.6% | `__vdso_clock_gettime` | 29.4% |
| `MapBook::cancel_order` | 22.2% | `HeapBook::add_order` | 11.4% | `OrderTable::insert` | 21.0% |
| `__vdso_clock_gettime` | 11.1% | `__vdso_clock_gettime` | 9.6% | `run_once<ArrayBook>` (add inlined here) | 11.4% |
| `run_once<MapBook>` | 6.1% | `unordered_map<tick>::operator[]` | 6.6% | `steady_clock::now` | 7.1% |
| `_Hashtable::_M_rehash` | 5.5% | `_Rb_tree::_M_erase` (order map destructor) | 5.1% | `play<ArrayBook>` (add inlined here) | 6.2% |
| libc (malloc / free) | 5.0% | `unordered_map<tick>::find` | 4.6% | `ArrayBook::cancel_order` | 5.8% |
| | | `_Rb_tree_rebalance_for_erase` | 4.5% | `vector<Slot>::_M_fill_assign` | 3.2% |
| | | `_Rb_tree_insert_and_rebalance` | 3.1% | `llround` | 1.4% |
| | | libc (malloc / free / memcpy) | 7.8% | kernel `clear_page_erms` | 0.8% |

In A and B the `std::map` / `std::unordered_map` code is mostly inlined into
`add_order`/`cancel_order`, so those two lines are mostly tree walking and hashing. In C
basically all the time outside the clock is in the hash table (`insert`, and the probe loop in
`cancel_order`). `_M_fill_assign` and the kernel's `clear_page_erms` are a new, bigger table being
zeroed every time it doubles. C has no malloc/free above 0.5% in its profile (the only
allocations left are the `std::vector`s returned by `get_bids`/`get_asks`).

## 4. Performance analysis (the questions from section 11 of the assignment)

### 11.1 Which implementation is fastest?

**C, for every operation**: 4.1x faster than A and 4.5x faster than B in total time, and 4.6x
fewer cycles per run than A.

- **add_order: C (47 ns) < B (73 ns) < A (116 ns).** C does one hash probe into a flat array and
  one `qty_[i] += qty`, no allocation and no tree. B beating A surprised us. The ids are
  increasing, so every insert into B's `std::map<id>` walks down the right edge of the tree,
  which is always in cache. Its level table has at most 5000 entries (fits in L2), and the heap
  push only happens when a level gets its first order. A inserts into a big `unordered_map`
  (random bucket, one `malloc` per order, rehashing as it grows: `_M_rehash` alone is 5.5% of
  A's samples) and then walks a ~13 level `std::map` where every comparison depends on a random price.
- **cancel_order: C (30 ns) << A (234 ns) < B (421 ns).** B searches for a random id in a
  red-black tree of ~500k nodes: ~19 levels, each one a pointer into ~30 MB of heap, so most of
  them are cache misses, plus a rebalance on erase. A's hash lookup is only 2-3 dependent misses,
  but then it also walks the level tree and frees the node. C touches one 16 byte slot (which
  already has the tick and qty) and the 80 KB qty array, which stays in L2.
- **get_bbo: C (1.5 ns) < A (1.7 ns) < B (3.1 ns).** None of them search: C reads two cached
  indexes, A reads the cached `begin()` of each map, B reads the heap tops but needs a hash lookup
  per side for the qty. Most of A's and C's cost is turning ticks back into `double` (a division
  by 100.0). In our first version of the timing loop the prices weren't used, the compiler
  dropped the conversion, and C's `get_bbo` came out at ~0.2 ns.

### 11.2 Branch prediction

**A has the most branch misses**: 93M in the perf stat run, 7.7M per run, 5.8-6.7% of branches.
Most come from the level `std::map`. At every node the CPU has to guess "left or right", and with
a uniformly random price that is a coin flip at each of ~13 levels, on every add and every
cancel. The `unordered_map` adds more on top (walking the bucket chain, the rehash check).

B has a lower rate (2.8%) but still 4.9M misses per run: its cancel does the same coin flip walk
(~19 levels) in the id tree, but its adds always go right, which pulls the rate down.

C has the fewest (0.99M per run, ~1 per op), and we think most of those are paid by every
implementation. The op type is random (70/20/10), so the benchmark's `switch` mispredicts often
(even always guessing "add" is wrong 30% of the time), and in `add_order` the side is a coin flip.
The rest is the exit of the probing loop (cluster length varies). C's rate looks higher with
`--no-timers` (4.8%) only because it runs so few branches that the workload generator's
branches are a bigger share.

### 11.3 Cache behaviour

**B has the most cache misses** (8.1M per run, 28.1M L1 misses per run), then A (5.3M / 17.2M),
then C (1.2M / 3.6M).

| | A | B | C |
|---|---|---|---|
| contiguous memory? | no | only the heap vector | yes, everything is a flat array |
| jumps around memory? | yes | yes | only the random hash slot |
| follows pointers? | yes: hash chain + tree nodes | yes: ~19 tree nodes per cancel | no |
| many small allocations? | yes: 1 node per order + 1 per level | yes: 1 node per order + 1 per level | no, 0 allocations per op (only on resize) |

Node based containers spread orders all over the heap. In B's tree the CPU has to load a node
before it knows the address of the next one, so the misses can't overlap. That's the textbook
pointer chasing problem and the reason B's cancel is the slowest op in the table. A chases fewer
pointers (bucket -> node) but still does a `malloc`/`free` per order, and a ~16 byte order becomes
a ~48 byte node. C has a single random access per add/cancel (a slot in a 16 MB table) and its
level array (80 KB) and bitmap (1.2 KB) stay in L1/L2. dTLB misses are close for A and C (0.60M vs
0.43M per run, both touch random 4 KB pages, C's table is 4096 pages) and highest for B (1.05M).

### 11.4 Page faults

**C causes the most page faults** (25,152 vs 19,610 for A and 18,787 for B), even though it uses
the least memory per order (~32 B vs ~48+ B per node in A/B).

1. They are a **one time cost**. `--runs 1` and `--runs 5` give the same count for all three
   (A 19,610 vs 19,613, B 18,790 vs 18,790, C 25,150 vs 25,153), because glibc reuses the freed
   memory for the next book. About 8k of every count is just the 32 MB op list.
2. We think C's extra faults come from **how its table grows** (64K -> 128K -> ... -> 1M slots).
   Every doubling allocates and zeroes a brand new array (`_M_fill_assign` and the kernel's
   `clear_page_erms` in the profile), so it touches 1 + 2 + 4 + 8 + 16 = 31 MB of new pages to end
   up with a 16 MB table. The node containers grow one small `malloc` at a time and reuse freed nodes.

Pre-sizing C's table to 2^21 slots from the start (`-DORDER_TABLE_INIT_CAP=2097152`,
`results/experiment_presize.txt`) made it **worse**: 101k page faults instead of 25k, 42.9 ms
instead of 27.2 ms, add 105 ns instead of 42 ns. Each of the 11 books the bench creates now
`mmap`s and zeroes a fresh 32 MB (11 x 8192 pages), and the emptier table spreads the same orders
over twice as many cache lines and pages. Pre-allocation only pays off if it's done once and reused.

### 11.5 Production choice

We would deploy **C**, with a couple of changes.

- **Latency**: lowest on every op. Cancel is ~8x faster than A, and cancels/modifies are most of
  the messages in a real market.
- **Predictability**: a small fixed amount of work per op (one probe, one array write, at worst a
  79 word bitmap scan). A and B depend on tree depth, cache state and `malloc`. C's one spike is
  the resize (the last doubling re-inserts 262k orders), so in production we would size the table
  once at startup, touch all its pages before trading, and never resize during trading.
- **Memory**: levels are a fixed 80 KB, orders are 16 B with no per-node allocator overhead, and
  with a pre-sized table nothing gets allocated while trading.
- **Cache behaviour**: ~5x fewer L1 misses than A per run. The hot part (qty array, bitmap, best
  indexes) fits in L1/L2.
- **Complexity / maintainability**: C's weak point. A is ~100 lines of standard containers that
  are hard to get wrong. C has a hand written hash table (backward shift delete is easy to get
  subtly wrong) and bit tricks. ~250 lines that are checked op by op against a brute force book
  (and a mutation test) is still manageable for us. C also depends on the price band being small
  and fixed. An instrument that can trade anywhere would need a sliding or re-centred array,
  while A works for any price.

Not B: it's the hardest to reason about (stale entries, `in_heap`), and it has the slowest depth
queries and the slowest cancel. If latency didn't matter we would pick A because it's the simplest.

### 11.6 What would we try next?

- **Integer tick API**: `llround` + the grid check are ~1.4% of C's samples, and tick -> double
  is most of `get_bbo` (11.1).
- **Size the order table once, pre-fault it, use huge pages** (`madvise(MADV_HUGEPAGE)`): 16 MB is
  4096 normal pages but only 8 huge pages, which should remove most of the dTLB misses.
- **Prefetch** the hash slot (`__builtin_prefetch`) as soon as the id is known.
- **Smaller slots** (32 bit qty, 16 bit level index, 48 bit id -> 8-12 bytes), so more slots fit
  in a cache line.
- **Order pool + intrusive FIFO list per level**, needed anyway once there's matching
  (price-time priority). The hash table would then map id -> pool index.
- **Tail latency** (p50/p99/p99.9/max with `rdtsc`) instead of averages, which hide exactly the
  resize stall in C and slow `malloc` calls in A/B.
- **Bare metal, pinned to one core, fixed CPU frequency** instead of a laptop inside WSL2.
- `get_bids`/`get_asks` filling a caller-provided array instead of returning a `std::vector`.
