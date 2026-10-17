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
| `bench.cpp` | workload generator, `--verify` correctness test, benchmark |
| `run_perf.sh` | builds, runs verify + bench + `perf stat` + `perf record` for A/B/C, writes `results/` |
| `per_run_counters.sh` | perf counters for one run of the book only (see REPORT) |
| `perf.Dockerfile` | the Ubuntu image we used for g++ and perf |
| `results/` | all the raw output we quote in the report |

## Build

Needs g++ with C++17 (we used g++ 13.3).

```
g++ -std=c++17 -O2 -Wall -Wextra -g -fno-omit-frame-pointer -o bench bench.cpp
```

or just `make`. `-g -fno-omit-frame-pointer` are only there so `perf record -g`
gives proper call stacks, the code is the same `-O2` build.

## Correctness test

```
./bench --verify
```

This runs three things on each implementation:
1. fixed edge cases: empty book, cancel of unknown id, cancel twice, bad prices
   (outside the band, wrong side of mid, not on the 0.01 grid), qty <= 0,
   duplicate id, fewer levels than requested depth, best level emptying, etc.
2. a random 200k op workload (70/20/10 mix, 5% of cancels go to unknown or
   already cancelled ids)
3. a "thin book" 200k op workload where adds ~= cancels, so levels keep
   emptying and the best price moves around a lot

For 2 and 3 every single op is checked against a brute force reference book
(plain vector of qty per tick and a full scan for every query): return value
of add/cancel, the BBO, top 10 bids and top 10 asks, and every 1000 ops the full
depth of both sides. At the end it runs the real 1M op benchmark workload through
A, B and C and checks the checksums are equal.

Our output (`results/verify.txt`):

```
Running correctness test...

1. edge cases (empty book, unknown/double cancel, bad prices, short depth, ...)
2. random workload: 200000 ops (add 140191, cancel 39805, bbo 20004), seed 7
3. thin book workload: 200000 ops (add 90067, cancel 89931, bbo 20002), seed 8
in 2 and 3 every op is compared against a brute force reference book

Implementation A: PASS
Implementation B: PASS
Implementation C: PASS

benchmark workload checksum: A=13740035995088966065 B=13740035995088966065 C=13740035995088966065 (match)

All implementations produce identical results.
```

We also ran `--verify` with `-fsanitize=address,undefined`, no errors.

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
- pass 2 wraps every add / cancel / get_bbo in `steady_clock::now()` and gives the
  **average ns per op type**. The cost of one `now()` call (~18 ns here) is
  measured at startup and subtracted, the raw number is printed too.

We run 5 times and print the median. At the end the final BBO and a checksum
(mixed from every cancel result, BBO and depth snapshot) are printed so the compiler
can't throw anything away. Example (`results/bench_C.txt`):

```
median of 5 runs:
  total time      30.88 ms  (30.9 ns per op, untimed loop)
  ns / add         43.9  (raw 61.8)
  ns / cancel      26.8  (raw 44.7)
  ns / bbo          0.1  (raw 17.9)

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

and saves everything in `results/`.

## Interface changes

The API from the assignment is kept (double prices in, double prices out), with
these changes:

- `add_order` returns `bool` instead of `void`. It returns false and leaves the book
  unchanged if the price is not on the 0.01 grid, outside the band for that side
  (bids 50.00-99.99, asks 100.01-150.00), qty is not in 1..2^31-1, or the id is
  already live.
- Internally every book uses integer ticks (`price * 100`, `llround`), so there's
  no floating point comparison anywhere in the books. Conversion happens once at
  the API boundary.
- `BBO` fields have default values, so an empty side is `has_x = false, price 0, qty 0`.
- Each book also has `size_t order_count() const` (only used to print the final state).
- There is no virtual base class. `bench.cpp` uses templates (`run_once<Book>`),
  so all three are used through the same 5 functions without a virtual call
  per op.

## Data structure choices (short version)

| | A | B | C |
|---|---|---|---|
| orders | `std::unordered_map<id, Order>` | `std::map<id, Order>` | own open addressing table, 16 byte slots, linear probing |
| price levels | `std::map<tick, qty>` per side | `std::unordered_map<tick, {qty, in_heap}>` per side | `long qty[10001]` indexed by tick - 5000 |
| best bid/ask | `begin()` of the map | top of `std::priority_queue`, stale tops popped on cancel | cached index, bitmap scan when the best level empties |
| cancel lookup | hash lookup | tree lookup O(log n) | hash lookup, no pointers |

| op | A | B | C |
|---|---|---|---|
| add_order | O(log P) | O(log N) | O(1) amortized |
| cancel_order | O(log P) | O(log N) + amortized O(log P) | O(1) expected, O(P/64) worst |
| get_bbo | O(1) | O(1) | O(1) |
| get_bids / get_asks | O(d) | O(P + d log P) | O(d + P/64) worst |

N = live orders, P = price levels per side (max 5000), d = depth. Details for each
in [REPORT.md](REPORT.md#1-the-three-implementations).
