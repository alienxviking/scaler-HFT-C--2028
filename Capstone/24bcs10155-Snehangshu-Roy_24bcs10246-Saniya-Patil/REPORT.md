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
- **Cancel**: hash lookup of the id -> we get side, tick, qty -> `map.find(tick)`,
  subtract, erase the level node if it hits 0.

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
| get_bids / get_asks | **O(P + d log P)** (copy the heap, then pop) |

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
  max load 0.5, doubles when full. `tick == 0` marks an empty slot. Deletion uses
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
- every counter from the assignment was available (multiplexed ~85% of the time)

It's a laptop inside a VM, so there's some noise between runs (the `+-` column in
perf stat goes up to ~6%). The ranking never changed between runs though.

## 3. Results table

Benchmark numbers are medians of 5 runs from `./bench --impl X` (`results/bench_X.txt`).
perf numbers are from `perf stat -r 5 ... ./bench --impl X` exactly as in the assignment
(`results/perf_stat_X.txt`).

| Metric | Implementation A | Implementation B | Implementation C |
|---|---|---|---|
| Total time (ms) | 116.1 | 122.7 | **30.9** |
| ns / add | 110.1 | 68.1 | **43.9** |
| ns / cancel | 223.8 | 311.6 | **26.8** |
| ns / BBO | ~0 (raw 18.1) | 4.7 (raw 22.6) | ~0 (raw 17.9) |
| IPC | 0.80 | 0.90 | **1.04** |
| Branch misses (%) | 6.00% (93.4M) | 2.92% (62.8M) | 3.40% (21.4M) |
| L1-dcache-load-misses | 192.3M | 302.9M | **46.0M** |
| Cache misses | 57.8M | 99.2M | **17.2M** |
| Page faults | 19,609 | 18,788 | 25,153 |
| dTLB-load-misses | 5.33M | 13.35M | 5.78M |

The ns/op numbers are with the ~18 ns `steady_clock::now()` overhead subtracted. `get_bbo`
in A and C is below what the clock can resolve. The raw time is the same as an empty
pair of `now()` calls. B's BBO is the only one we can actually see, because it does two hash
lookups to get the qty.

The per-op numbers (timed one call at a time) add up to more than the total time. Timing
every op on its own means the CPU can't overlap one op's cache misses with the next op's work,
so per-op = latency, total / ops = throughput.

### Cleaner counters

When we looked at `perf record` we saw `__vdso_clock_gettime` taking 10% of A, 10% of B and
**38% of C's** samples. The per-op timing pass was polluting the counters (and it was
pushing C's IPC up, because the clock code itself runs at a high IPC). The perf stat for the whole program also
includes generating the 1M ops and the final replay, which is the same work for all three.
So we did two more things:

1. `--no-timers` runs only the untimed pass (`results/perf_stat_X_no_timers.txt`)
2. `per_run_counters.sh` runs `perf stat` with `--runs 1` and `--runs 5` (both `--no-timers`)
   and computes `(runs5 - runs1) / 4`. That's the cost of **one pass of 1M ops on one book**,
   including creating and destroying the book, with all the shared stuff cancelled out
   (`results/perf_per_run.txt`):

| per run (1M ops) | A | B | C |
|---|---|---|---|
| cycles | 733M | 846M | **155M** |
| instructions | 543M | 784M | **204M** |
| IPC | 0.74 | 0.93 | **1.31** |
| branches | 119.7M | 174.7M | **32.9M** |
| branch-misses | 7.87M (6.6%) | 4.69M (2.7%) | **1.08M (3.3%)** |
| cache-misses | 5.09M | 8.01M | **1.12M** |
| L1-dcache-load-misses | 17.6M | 28.5M | **3.6M** |
| dTLB-load-misses | 0.50M | 1.02M | 0.49M |
| page-faults | 0 | 0 | 0 |

We use this table for most of the analysis below.

### perf stat output

`perf stat -r 5 -e task-clock,cycles,instructions,branches,branch-misses,cache-misses,L1-dcache-load-misses,dTLB-load-misses,page-faults ./bench --impl X`

**A**
```
 Performance counter stats for './bench --impl A' (5 runs):

           1921.17 msec task-clock                       #    0.999 CPUs utilized               ( +-  2.74% )
        9285989401      cycles                           #    4.833 GHz                         ( +-  2.40% )  (85.57%)
        7404136173      instructions                     #    0.80  insn per cycle              ( +-  0.18% )  (85.70%)
        1557510296      branches                         #  810.707 M/sec                       ( +-  0.24% )  (85.78%)
          93425210      branch-misses                    #    6.00% of all branches             ( +-  0.14% )  (85.75%)
          57758185      cache-misses                                                            ( +-  1.49% )  (85.80%)
         192250028      L1-dcache-load-misses                                                   ( +-  0.21% )  (85.76%)
           5328492      dTLB-load-misses                                                        ( +-  9.25% )  (85.64%)
             19609      page-faults                      #   10.207 K/sec                       ( +-  0.01% )

            1.9239 +- 0.0526 seconds time elapsed  ( +-  2.73% )
```

**B**
```
 Performance counter stats for './bench --impl B' (5 runs):

           2569.94 msec task-clock                       #    0.999 CPUs utilized               ( +-  6.46% )
       11091107983      cycles                           #    4.316 GHz                         ( +-  3.91% )  (85.60%)
        9948509718      instructions                     #    0.90  insn per cycle              ( +-  0.13% )  (85.64%)
        2152494624      branches                         #  837.565 M/sec                       ( +-  0.07% )  (85.76%)
          62823004      branch-misses                    #    2.92% of all branches             ( +-  3.51% )  (85.76%)
          99244356      cache-misses                                                            ( +-  5.21% )  (85.76%)
         302926453      L1-dcache-load-misses                                                   ( +-  1.16% )  (85.74%)
          13354189      dTLB-load-misses                                                        ( +-  3.80% )  (85.74%)
             18788      page-faults                      #    7.311 K/sec                       ( +-  0.01% )

             2.573 +- 0.166 seconds time elapsed  ( +-  6.46% )
```

**C**
```
 Performance counter stats for './bench --impl C' (5 runs):

            917.77 msec task-clock                       #    0.997 CPUs utilized               ( +-  6.26% )
        3634484241      cycles                           #    3.960 GHz                         ( +-  2.59% )  (85.69%)
        3774790367      instructions                     #    1.04  insn per cycle              ( +-  0.47% )  (85.67%)
         629336143      branches                         #  685.724 M/sec                       ( +-  0.52% )  (85.64%)
          21412932      branch-misses                    #    3.40% of all branches             ( +-  1.63% )  (85.70%)
          17249503      cache-misses                                                            ( +-  6.88% )  (85.65%)
          45968015      L1-dcache-load-misses                                                   ( +-  1.49% )  (85.83%)
           5783509      dTLB-load-misses                                                        ( +-  5.60% )  (85.83%)
             25153      page-faults                      #   27.407 K/sec                       ( +-  0.00% )

            0.9207 +- 0.0574 seconds time elapsed  ( +-  6.23% )
```

### perf record (top of `perf report --no-children`, full output in `results/perf_report_X.txt`)

| A | | B | | C | |
|---|---|---|---|---|---|
| `MapBook::add_order` | 34.3% | `HeapBook::cancel_order` | 25.6% | `__vdso_clock_gettime` | 38.5% |
| `MapBook::cancel_order` | 22.9% | `HeapBook::add_order` | 11.8% | `OrderTable::insert` | 23.8% |
| `__vdso_clock_gettime` | 10.3% | `__vdso_clock_gettime` | 10.1% | `run_once<ArrayBook>` (add inlined here) | 10.8% |
| `_Hashtable::_M_rehash` | 6.5% | `unordered_map<tick>::operator[]` | 6.2% | `ArrayBook::cancel_order` | 6.1% |
| `run_once<MapBook>` | 6.1% | `unordered_map<tick>::find` | 5.4% | `vector<Slot>::_M_fill_assign` | 2.4% |
| malloc + free | ~1.5% | `_Rb_tree_rebalance_for_erase` | 5.4% | `llround` | 1.6% |
| | | `_Rb_tree::_M_erase` (map destructor) | 5.1% | | |
| | | `_Rb_tree_insert_and_rebalance` | 3.4% | | |

In A and B the `std::map` / `std::unordered_map` code is mostly inlined into
`add_order`/`cancel_order`, so those two lines are mostly tree walking and hashing. In C
basically all the time outside the clock is in the hash table (`insert` and the probe loop in
`cancel_order`). `_M_fill_assign` is zeroing a new, bigger table every time it doubles.

## 4. Analysis

### 4.1 Which implementation is fastest?

**C is fastest at everything**: 3.8x faster total than A, 4x faster than B, and 4.7x
fewer cycles per run than A.

- **add_order: C (44 ns) < B (68 ns) < A (110 ns).** C does one hash probe into a flat array
  and one `qty_[i] += qty`. There's no allocation and no tree. The surprise for us was that B
  beats A on add even though B uses a tree for orders. The ids in the benchmark are increasing,
  so every insert into B's `std::map<id>` goes down the right edge of the tree, and those nodes
  are always hot in cache. B's level lookup is a hash table of at most 5000 entries (fits in
  L2), and the heap push almost never happens because all 5000 levels exist after the first
  few thousand adds. A instead inserts into a large `unordered_map` (random bucket in an array of
  hundreds of thousands of buckets, one `malloc` per order, rehashing as it grows: `_M_rehash` alone is 6.5% of
  A's samples) and then walks a 13-level `std::map` of levels where every comparison depends on
  a random price.
- **cancel_order: C (27 ns) << A (224 ns) < B (312 ns).** This is where the biggest difference
  is. B has to find a random id in a red-black tree of ~500k nodes: about 19 levels, each one a
  pointer to a node somewhere in ~30 MB of heap, so most levels below the top few are a
  cache miss, plus a rebalance on erase. A's hash lookup is only ~2 dependent misses (bucket,
  node), but then it also walks the level tree and frees the node. C's cancel is one slot
  in the table (the 16 byte slot holds everything we need) and one access to the 80 KB qty array,
  which stays in L2.
- **get_bbo: A and C are both below what we can measure (<1 ns),** they just read a cached
  value (`map.begin()` / `best_bid_`). B needs a hash lookup per side to get the qty, so it's the
  only one that shows up (~5 ns).

### 4.2 Branch prediction

**A has the most branch misses**, both in total (93M, 7.9M per run) and as a rate (6-6.7%).
Most of them come from `std::map::find` / `operator[]` on the levels. At every node the CPU has
to guess "go left or right", and since the price is uniformly random, that guess is basically a
coin flip at each of ~13 levels, and A does this on every add and every cancel. The
`unordered_map` adds more on top: walking the bucket chain, and the "do we need to rehash"
check.

B has a lower *rate* (2.7%) but still 4.7M misses per run. The id tree lookups on cancel
are random (same left/right coin flip, ~19 levels), but its adds are very predictable (always
go right), which pulls the percentage down.

C has the fewest misses (1.08M per run, about 1 per op). We think a big part of that is not
even the book: the benchmark's `switch` on the op type is random (70/20/10), so it
mispredicts quite often for all three books. The rest is the linear probing loop (cluster
length is random, so the loop exit is hard to predict) and the `if (i > best_bid_)` check.
C's *rate* looks higher in the `--no-timers` run (5%) only because it runs so few branches in
total that the workload generator's branches are a bigger share.

### 4.3 Cache behaviour

**B has the most cache misses** (8.0M per run, 28.5M L1 misses per run), then A (5.1M / 17.6M),
then C (1.1M / 3.6M). Per the questions:

| | A | B | C |
|---|---|---|---|
| contiguous memory? | no | only the heap vector | yes, everything is a flat array |
| jumps around memory? | yes | yes | only the random hash slot |
| follows pointers? | yes: hash chain + tree nodes | yes: ~19 tree nodes per cancel | no |
| many small allocations? | yes: 1 node per order + 1 per level | yes: 1 node per order + 1 per level | no, 0 allocations per op (only on resize) |

The node-based containers spread orders all over the heap. To find one order in B's tree
the CPU has to load each node before it knows the address of the next one, so the misses can't
overlap. That's the textbook pointer chasing problem, and that's why B's cancel is the slowest
op in the whole table. A has fewer levels of pointer chasing (hash bucket -> node) but still a
`malloc`/`free` per order and node headers that waste cache space (a ~16 byte order ends up as a
~48 byte node).

C still has one random access per add/cancel (the hash slot in a 16 MB table, bigger than L2),
but that's a single miss, and the slot already has the tick and qty in it. The level array
(80 KB) and the bitmap (1.2 KB) are small enough to stay in L1/L2 the whole time. dTLB misses
are about the same for A and C (~0.5M per run). Both touch random 4 KB pages (C's table is 16 MB
= 4096 pages). B is double that.

### 4.4 Page faults

**C causes the most page faults** (25,153 vs 19,609 for A and 18,788 for B). This was not
what we expected, since C uses the least memory per order (16 byte slot, at most 50% full,
so ~32 B/order vs ~48+ B per node in A/B).

Two things we found:

1. Page faults are a **one time cost**. `--runs 1` and `--runs 5` give the same number of
   faults (per-run delta is 0 for all three). After the first run glibc keeps the freed memory
   and gives it back to the next book, so the later runs never touch new pages. About 8k of the faults
   in every implementation are just the 32 MB op list.
2. C's extra faults come from **how its table grows**: 64K -> 128K -> ... -> 1M slots. Every
   doubling allocates a brand new bigger array and zeroes it (`_M_fill_assign` in the profile
   and `clear_page_erms` in the kernel), so it touches 1 + 2 + 4 + 8 + 16 = 31 MB of new
   pages to end up with a 16 MB table. The old smaller arrays can't be reused for the bigger one.
   The node containers grow one small `malloc` at a time and reuse freed nodes, so they touch
   about as much memory as they actually use.

We also tried just pre-sizing C's table to 2^21 slots (32 MB) from the start
(`results/experiment_presize.txt`). It got **worse**: 101k page faults and 49.8 ms instead of
30.9 ms. Every new book now `mmap`s and zeroes 32 MB, and since the bench creates 11 books
that's 11 x 8192 fresh pages. On top of that the bigger, emptier table spreads the same orders
over twice as many cache lines and pages. So pre-allocation only helps if it's done **once**
and the memory is reused, not when every new object allocates a big block for itself.

### 4.5 Production choice

We would deploy **C**, with a couple of changes (below).

- **Latency**: lowest on every op, and cancel is ~8x faster than A. In a real market most
  messages are cancels/modifies, so that matters more than add.
- **Predictability**: every op is a fixed number of steps: one hash probe, one array write,
  maybe a bitmap scan of at most 79 words. A and B depend on tree depth, on what's in cache
  and on `malloc`, which can take a slow path at any time. The one unpredictable thing in C is
  the table resize: the add that triggers the last doubling has to re-insert every live order
  (262k of them). In production we'd size the table once at startup for the max number of orders we
  expect, touch all the pages before the market opens, and never resize during trading.
- **Memory**: price levels are fixed at 80 KB no matter how busy the book is. Orders are 16 B
  each with no per-node malloc overhead. The memory is all allocated up front, so the
  allocator isn't involved during trading.
- **Cache behaviour**: best of the three by far (~5x fewer L1 misses than A per run). The hot
  part of the book (qty array + bitmap + best indexes) fits in L1/L2.
- **Complexity / maintainability**: this is C's weak point. A is ~100 lines of standard containers
  that anyone can read and is hard to get wrong. C has a hand written hash table (backward
  shift delete is easy to get subtly wrong) and bit tricks. We think it's still manageable
  because the whole thing is ~250 lines and `--verify` compares it op by op against a brute
  force book. It also only works because the price band is small and fixed. A real instrument
  that can trade anywhere would need the array to slide or be re-centred, while A works for any price.

We would not pick B: the lazy deletion heap is the most complicated to reason about (stale
entries, `in_heap` flag), depth queries are the slowest (copy + pop), and its cancel is the
worst. If latency didn't matter we'd pick A for simplicity.

### 4.6 What we would try next

- **Integer tick API.** `llround` + the grid check still shows up (~1.6% of C) and the double
  conversion is pointless if the feed already gives ticks.
- **Size the order table once and pre-fault it**, maybe with huge pages (`madvise(MADV_HUGEPAGE)`
  or a 2 MB-page `mmap`). 16 MB is 4096 normal pages but only 8 huge pages, which should
  get rid of most of the dTLB misses. Our pre-size experiment showed this only works if it's
  done once, not per object.
- **Prefetch** the hash slot (`__builtin_prefetch`) as soon as we know the id. In a real
  system the id is known a bit before we need the slot.
- **Smaller slots**: qty fits in 32 bits and the level index in 16, so the slot could be 12 bytes or
  even 8 with a 48 bit id, more slots per cache line.
- **Order pool + intrusive FIFO list per level**: needed anyway once there is matching
  (price-time priority). Orders would live in a pre-allocated pool and the hash table would just
  map id -> pool index.
- **Measure tail latency** (p50/p99/p99.9 and max) with `rdtsc` instead of averages. An average hides
  exactly the things that matter in production, like the resize stall in C or a slow `malloc` in A/B.
- **Run on bare metal, pinned to one core** (`taskset`, fixed CPU frequency). Our numbers come from a laptop
  inside WSL2, so there's turbo, VM and background noise in them.
- `get_bids` / `get_asks` could fill a caller-provided fixed-size array instead of returning a
  `std::vector`, so there's no allocation per depth query.
