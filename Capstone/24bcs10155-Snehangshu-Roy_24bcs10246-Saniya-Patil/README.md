# Capstone: Low-Latency Order Book

Team:
- Snehangshu Roy, 24BCS10155, snehangshu.24bcs10155@sst.scaler.com
- Saniya Patil, 24BCS10246, saniya.24bcs10246@sst.scaler.com

The same single-instrument resting order book, built three times with
different data structures, plus one benchmark program that runs all of them on the
same workload. The full results table and the analysis are in [REPORT.md](REPORT.md).

## Files

| file | what |
|---|---|
| `common.hpp` | `Side`, `Level`, `BBO`, tick constants, price validation shared by all books |
| `map_book.hpp` | Implementation A: `std::map` levels + `std::unordered_map` orders |
| `heap_book.hpp` | Implementation B: `std::priority_queue` with lazy deletion + `std::map` orders |
| `array_book.hpp` | Implementation C (our design): flat array of levels + bitmap + own open addressing hash table |
| `workload.hpp` | random op generator (fixed seed) and the loop that plays it on a book + checksum |
| `verify.hpp` | `--verify`: edge cases, brute force reference book, random workloads on A/B/C side by side |
| `bench.cpp` | `main`, the benchmark (`--impl A/B/C`) and timing |
| `Makefile` | `make` builds `bench`, `make verify` runs the correctness test |
| `run_perf.sh` | builds, runs verify + bench + `perf stat` + `perf record` for A/B/C, writes `results/` |
| `per_run_counters.sh` | perf counters for one run of the book only (see REPORT) |
| `perf.Dockerfile` | the Ubuntu image we used for g++ and perf |
| `results/` | all the raw output we quote in the report |
| `tests/` | extra checks: build matrix with sanitizers, and a mutation test for `--verify` |

## Build

Needs g++ (or clang++) with C++17. We used g++ 13.3.

```
g++ -std=c++17 -O2 -Wall -Wextra -g -fno-omit-frame-pointer -o bench bench.cpp
```

or just `make`. `-g -fno-omit-frame-pointer` are only there so `perf record -g`
gives proper call stacks, the code is the same `-O2` build. Plain
`g++ -std=c++17 -O2 -o bench bench.cpp` works too.

Only the standard library is used. C also uses `__builtin_clzll` / `__builtin_ctzll`
(GCC/Clang compiler builtins, not a library) to find set bits in its bitmap.

## Correctness test

```
./bench --verify
```

What it does (code in `verify.hpp`):
1. fixed edge cases on each book: empty book, cancel of an unknown id, cancel twice,
   bad prices (outside the band, wrong side of the mid, not on the 0.01 grid, NaN,
   inf), qty <= 0 or too big, duplicate id, id 0 and the max id, fewer levels than the
   requested depth, depth 0 / negative, a level in the middle emptying, a level coming
   back after it emptied, etc.
2. C's hand written hash table on its own against `std::unordered_map`
   (9 x 200k random inserts/erases, starting from 16 slots so it resizes a lot)
3. four random workloads of 200k ops each: the normal 70/20/10 mix, a "thin" book
   where adds ~= cancels so levels keep emptying, random 64 bit ids instead of
   1, 2, 3, ..., and random ids with all prices in 20 ticks per side.
   5% of cancels go to unknown or already cancelled ids.
   Each workload is played on A, B, C and a brute force reference book (plain array
   of qty per tick, full scan for every query) side by side, and after **every op**
   all three are compared with the reference: the add/cancel return value, the BBO,
   top 10 bids and asks, and the bids and asks at a second depth that changes
   every op (-2 to 22, and the whole book every 1000 ops).
4. the real 1M op benchmark workload on A, B and C. The checksums have to match.

Our output (`results/verify.txt`):

```
Running correctness test...

1. edge cases: empty book, unknown / double cancel, bad prices and qty, id 0 and max,
   short depth, levels emptying and coming back
2. C's OrderTable vs std::unordered_map, 9 x 200000 random inserts/erases: ok
3. random workloads, A, B, C and a brute force book side by side, compared after every op
   (5% of cancels go to unknown or already cancelled ids)
   a) normal 70/20/10 mix: 200000 ops (add 140191, cancel 39805, bbo 20004), seed 7
   b) thin book, 45% add / 45% cancel: 200000 ops (add 90067, cancel 89931, bbo 20002), seed 8
   c) random 64 bit ids: 200000 ops (add 140170, cancel 39925, bbo 19905), seed 9
   d) random ids, thin book, only 20 ticks per side: 200000 ops (add 89674, cancel 90430, bbo 19896), seed 10
4. benchmark workload (1M ops, seed 42) checksum: A=13740035995088966065 B=13740035995088966065 C=13740035995088966065 (match)

Implementation A: PASS
Implementation B: PASS
Implementation C: PASS

All implementations produce identical results.
```

Extra testing (scripts in `tests/`):
- `tests/check_builds.sh` builds with g++ (`-std=c++17/20/23`, `-O0/-O2/-O3`), clang++,
  AddressSanitizer + UBSan (g++ and clang++), `_GLIBCXX_ASSERTIONS` and `_GLIBCXX_DEBUG`
  (checked STL containers), all with `-Wall -Wextra -Wpedantic -Wshadow`, and runs `--verify`
  with each build. No warnings, no errors.
- `tests/mutation_test.sh` checks that the test itself works: it puts 14 known bugs into a
  copy of the code (one at a time, listed in `tests/mutants.txt`, e.g. not updating the best bid,
  skipping the backward shift in C's hash table, not cleaning B's heap top, keeping empty levels
  in A, accepting 100.00 as a bid) and runs `--verify`. All 14 get caught.

## Benchmark

```
./bench --impl A
./bench --impl B
./bench --impl C
```

Optional: `--runs N` (default 5), `--ops N` (default 1,000,000), `--seed S` (default 42),
`--no-timers` (skip the per-op timing pass, only total time).

Workload: 1M ops generated up front with `std::mt19937(42)`, ~70% add, ~20% cancel,
~10% `get_bbo`, plus `get_bids(10)` and `get_asks(10)` every 100 ops. Side is 50/50,
qty 1-100, price is a uniformly random tick in the band for that side. Cancels
always pick a currently live order (the generator tracks live ids). All three
implementations get the exact same op list.

Each run does two passes on a fresh book:
- pass 1 with no timers in the loop gives the **total time** and the checksum
- pass 2 wraps every add / cancel in `steady_clock::now()` and gives the
  **average ns per op type**. The cost of one `now()` call (~18 ns here) is
  measured at startup and subtracted, the raw number is printed too.
  One `get_bbo` is faster than one `now()` call, so for every BBO op we time 16
  `get_bbo` calls back to back and divide by 16. There's a compiler barrier
  (`std::atomic_signal_fence`) between the calls so the compiler can't merge them
  into one. We checked the assembly: all 16 calls are there and every one of them
  re-reads the book and does the tick to price conversion.

We run 5 times and print the median. At the end the final BBO and a checksum
(mixed from every cancel result, BBO and depth snapshot) are printed so the compiler
can't throw anything away. Example (`results/bench_C.txt`):

```
median of 5 runs:
  total time      30.71 ms  (30.7 ns per op, untimed loop)
  ns / add         47.5  (raw 65.4)
  ns / cancel      29.5  (raw 47.3)
  ns / bbo         1.52  (raw 2.63, timed in batches of 16 calls)

Final BBO:
Bid: 99.99 x 1981
Ask: 100.01 x 2573
Live orders: 501168

Checksum: 13740035995088966065
```

## perf

We are on Windows, so everything Linux was run in Docker Desktop (WSL2 kernel
6.6, Ubuntu 24.04, perf 6.8). All counters asked for in the assignment were
available (there's some multiplexing, ~85%, because there are more hardware events than
counters).

```
docker build -t ob-perf -f perf.Dockerfile .
docker run --rm --privileged -v "$PWD:/work" ob-perf ./run_perf.sh
```

`run_perf.sh` runs, for each of A/B/C:

```
perf stat -r 5 -e task-clock,cycles,instructions,branches,branch-misses,\
cache-misses,L1-dcache-load-misses,dTLB-load-misses,page-faults ./bench --impl X
perf record -g ./bench --impl X
perf report
```

and saves everything in `results/`. It also runs the same perf stat with `--no-timers`
(`perf_stat_X_no_timers.txt`), `per_run_counters.sh` (counters for one run of the book
alone, see REPORT section 3) and the pre-sized hash table experiment (`experiment_presize.txt`).

## Interface changes

The API from the assignment is kept (double prices in, double prices out), with
these changes:

- `add_order` returns `bool` instead of `void`. It returns false and leaves the book
  unchanged if the price is not on the 0.01 grid, outside the band for that side
  (bids 50.00-99.99, asks 100.01-150.00), qty is not in 1..2^31-1, or the id is
  already live.
- Internally every book uses integer ticks (`price * 100`, `llround`), so the data
  structures never compare doubles. The conversion and the grid check happen once
  in `add_order` (`price_to_tick` in `common.hpp`), and prices are converted back
  only when returning a `BBO` / `Level`.
- `BBO` fields have default values, so an empty side is `has_x = false, price 0, qty 0`.
- Each book also has `size_t order_count() const` (only used to print the final state).
- The three classes are called `MapBook` (A), `HeapBook` (B) and `ArrayBook` (C)
  instead of `OrderBook`. All three have the same 5 public functions with the same
  signatures. There's no virtual base class: the benchmark and verify code are
  templates (`play<Book>`, `run_once<Book>`, `edge_cases<Book>`), so all three go
  through exactly the same code without a virtual call per op.

## Data structure choices (short version)

| | A | B | C |
|---|---|---|---|
| orders | `std::unordered_map<id, Order>` | `std::map<id, Order>` | own open addressing table, 16 byte slots, linear probing |
| price levels | `std::map<tick, qty>` per side | `std::unordered_map<tick, {qty, in_heap}>` per side | `long qty[10001]` indexed by tick - 5000 |
| best bid/ask | `begin()` of the map | top of `std::priority_queue`, stale tops popped on cancel | cached index, bitmap scan when the best level empties |
| cancel lookup | chained hashing: bucket -> pointer to a separately allocated node | tree search, ~19 nodes deep for 500k orders | open addressing: probe 16 byte slots in one array, the order data is inside the slot |

| op | A | B | C |
|---|---|---|---|
| add_order | O(log P) | O(log N) | O(1) amortized |
| cancel_order | O(log P) | O(log N) + amortized O(log P) | O(1) expected, O(P/64) worst |
| get_bbo | O(1) | O(1) | O(1) |
| get_bids / get_asks | O(d) | O(P + (d + s) log P), worst O(P log P) | O(d + P/64) worst |

N = live orders, P = price levels per side (max 5000), d = depth, s = stale ticks
skipped. Details for each in [REPORT.md](REPORT.md#1-the-three-implementations).
