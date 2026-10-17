// Benchmark + correctness test for the three order books.
//
//   ./bench --verify              run edge case tests + random workload on A, B, C
//   ./bench --impl A|B|C          benchmark one implementation (5 runs, median)
//
// extra flags: --runs N, --ops N, --seed S, --no-timers (only total time)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "array_book.hpp"
#include "common.hpp"
#include "heap_book.hpp"
#include "map_book.hpp"

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- workload

enum class OpType : uint8_t { Add, Cancel, Bbo };

struct Op {
    OpType type;
    Side side;
    long qty;
    double price;
    uint64_t id;
};

struct WorkloadCounts {
    size_t add = 0, cancel = 0, bbo = 0;
};

// Generates the op list up front so every implementation sees exactly the
// same sequence and the RNG isn't part of the timed loop.
// 70% add / 20% cancel / 10% bbo. Cancels always pick a live order (we keep
// our own list of live ids while generating). If nothing is live yet the
// cancel becomes an add.
// bad_cancel_pct > 0 is only used by --verify: that % of cancels go to an id
// that is unknown or already cancelled, to check those return false.
// add_pct lets --verify make a "thin" book (adds ~= cancels) where levels
// empty out all the time.
std::vector<Op> make_workload(size_t n, uint32_t seed, int bad_cancel_pct = 0, int add_pct = 70) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> coin(0, 1);
    std::uniform_int_distribution<long> qty_dist(1, 100);
    std::uniform_int_distribution<int> bid_dist(MIN_BID_TICK, MAX_BID_TICK);
    std::uniform_int_distribution<int> ask_dist(MIN_ASK_TICK, MAX_ASK_TICK);

    std::vector<Op> ops;
    ops.reserve(n);
    std::vector<uint64_t> live, dead;
    uint64_t next_id = 1;

    for (size_t i = 0; i < n; i++) {
        int r = pct(rng);
        Op op{};
        if (r < add_pct || (r < 90 && live.empty())) {
            op.type = OpType::Add;
            op.side = coin(rng) ? Side::Buy : Side::Sell;
            int tick = op.side == Side::Buy ? bid_dist(rng) : ask_dist(rng);
            op.price = to_price(tick);
            op.qty = qty_dist(rng);
            op.id = next_id++;
            live.push_back(op.id);
        } else if (r < 90) {
            op.type = OpType::Cancel;
            if (bad_cancel_pct > 0 && pct(rng) < bad_cancel_pct) {
                // already cancelled id, or one we never handed out
                if (!dead.empty() && coin(rng))
                    op.id = dead[std::uniform_int_distribution<size_t>(0, dead.size() - 1)(rng)];
                else
                    op.id = next_id + 1000000 + rng() % 1000;
            } else {
                size_t k = std::uniform_int_distribution<size_t>(0, live.size() - 1)(rng);
                op.id = live[k];
                live[k] = live.back();
                live.pop_back();
                dead.push_back(op.id);
            }
        } else {
            op.type = OpType::Bbo;
        }
        ops.push_back(op);
    }
    return ops;
}

WorkloadCounts count_ops(const std::vector<Op>& ops) {
    WorkloadCounts c;
    for (const Op& op : ops) {
        if (op.type == OpType::Add) c.add++;
        else if (op.type == OpType::Cancel) c.cancel++;
        else c.bbo++;
    }
    return c;
}

// ---------------------------------------------------------------- checksum

inline void mix(uint64_t& cs, uint64_t v) { cs = cs * 1000003ULL + v; }

inline void mix_bbo(uint64_t& cs, const BBO& b) {
    if (b.has_bid) { mix(cs, to_tick(b.bid_price)); mix(cs, b.bid_qty); }
    if (b.has_ask) { mix(cs, to_tick(b.ask_price)); mix(cs, b.ask_qty); }
}

inline void mix_levels(uint64_t& cs, const std::vector<Level>& lv) {
    for (const Level& l : lv) { mix(cs, to_tick(l.price)); mix(cs, l.qty); }
}

// ---------------------------------------------------------------- benchmark

struct RunResult {
    double total_ms = 0;
    double add_ns = 0, cancel_ns = 0, bbo_ns = 0;  // raw, still includes one clock call
    uint64_t checksum = 0;
};

// rough cost of one steady_clock::now() call, so we can subtract it from the
// per-op numbers
double timer_overhead_ns() {
    const int n = 1000000;
    auto a = Clock::now();
    for (int i = 0; i < n; i++) {
        auto t = Clock::now();
        asm volatile("" : : "r"(&t) : "memory");
    }
    auto b = Clock::now();
    return std::chrono::duration<double, std::nano>(b - a).count() / n;
}

// Pass 1 runs the whole workload with no timers inside the loop -> total time
// and checksum. Pass 2 runs it again on a fresh book and wraps every
// add/cancel/bbo call in steady_clock::now() -> per-op averages.
// (doing both in one pass would make the total time mostly clock calls)
//
// --no-timers skips pass 2. We added it after perf record showed
// clock_gettime at ~37% of C's samples, which was messing up the counters.
bool g_op_timers = true;

template <class Book>
RunResult run_once(const std::vector<Op>& ops) {
    RunResult r;

    {
        auto book = std::make_unique<Book>();
        uint64_t cs = 0;
        auto start = Clock::now();
        for (size_t i = 0; i < ops.size(); i++) {
            const Op& op = ops[i];
            switch (op.type) {
                case OpType::Add: book->add_order(op.id, op.side, op.price, op.qty); break;
                case OpType::Cancel: cs += book->cancel_order(op.id); break;
                case OpType::Bbo: mix_bbo(cs, book->get_bbo()); break;
            }
            if ((i + 1) % 100 == 0) {
                mix_levels(cs, book->get_bids(10));
                mix_levels(cs, book->get_asks(10));
            }
        }
        auto end = Clock::now();
        mix_bbo(cs, book->get_bbo());
        r.total_ms = std::chrono::duration<double, std::milli>(end - start).count();
        r.checksum = cs;
    }
    if (!g_op_timers) return r;

    {
        auto book = std::make_unique<Book>();
        double add_ns = 0, cancel_ns = 0, bbo_ns = 0;
        size_t adds = 0, cancels = 0, bbos = 0;
        uint64_t sink = 0;
        for (size_t i = 0; i < ops.size(); i++) {
            const Op& op = ops[i];
            if (op.type == OpType::Add) {
                auto t0 = Clock::now();
                book->add_order(op.id, op.side, op.price, op.qty);
                auto t1 = Clock::now();
                add_ns += std::chrono::duration<double, std::nano>(t1 - t0).count();
                adds++;
            } else if (op.type == OpType::Cancel) {
                auto t0 = Clock::now();
                bool ok = book->cancel_order(op.id);
                auto t1 = Clock::now();
                sink += ok;
                cancel_ns += std::chrono::duration<double, std::nano>(t1 - t0).count();
                cancels++;
            } else {
                auto t0 = Clock::now();
                BBO b = book->get_bbo();
                auto t1 = Clock::now();
                sink += b.bid_qty + b.ask_qty;
                bbo_ns += std::chrono::duration<double, std::nano>(t1 - t0).count();
                bbos++;
            }
            if ((i + 1) % 100 == 0) {
                sink += book->get_bids(10).size();
                sink += book->get_asks(10).size();
            }
        }
        // keep sink alive
        if (sink == 42) std::printf(" ");
        r.add_ns = add_ns / adds;
        r.cancel_ns = cancel_ns / cancels;
        r.bbo_ns = bbo_ns / bbos;
    }
    return r;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

template <class Book>
int run_bench(const char* name, size_t n_ops, uint32_t seed, int runs) {
    std::vector<Op> ops = make_workload(n_ops, seed);
    WorkloadCounts c = count_ops(ops);
    double overhead = timer_overhead_ns();

    std::printf("Implementation %s\n", name);
    std::printf("ops: %zu (add %zu, cancel %zu, bbo %zu), seed %u, runs %d\n", ops.size(), c.add,
                c.cancel, c.bbo, seed, runs);
    std::printf("steady_clock::now() overhead: %.1f ns\n\n", overhead);

    std::vector<double> total, add, cancel, bbo;
    uint64_t checksum = 0;
    for (int k = 0; k < runs; k++) {
        RunResult r = run_once<Book>(ops);
        if (g_op_timers)
            std::printf("run %d: total %8.2f ms | raw add %6.1f ns | cancel %6.1f ns | bbo %5.1f ns\n", k + 1,
                        r.total_ms, r.add_ns, r.cancel_ns, r.bbo_ns);
        else
            std::printf("run %d: total %8.2f ms\n", k + 1, r.total_ms);
        total.push_back(r.total_ms);
        add.push_back(r.add_ns);
        cancel.push_back(r.cancel_ns);
        bbo.push_back(r.bbo_ns);
        if (k > 0 && r.checksum != checksum) {
            std::printf("checksum changed between runs?!\n");
            return 1;
        }
        checksum = r.checksum;
    }

    std::printf("\nmedian of %d runs:\n", runs);
    std::printf("  total time   %8.2f ms  (%.1f ns per op, untimed loop)\n", median(total),
                median(total) * 1e6 / ops.size());
    // raw = time between the two now() calls, net = raw - clock overhead
    auto line = [&](const char* what, double raw) {
        std::printf("  ns / %-7s %8.1f  (raw %.1f)\n", what, std::max(0.0, raw - overhead), raw);
    };
    if (g_op_timers) {
        line("add", median(add));
        line("cancel", median(cancel));
        line("bbo", median(bbo));
    }

    // replay once more to print the final state of the book
    auto book = std::make_unique<Book>();
    for (const Op& op : ops) {
        if (op.type == OpType::Add) book->add_order(op.id, op.side, op.price, op.qty);
        else if (op.type == OpType::Cancel) book->cancel_order(op.id);
    }
    BBO b = book->get_bbo();
    std::printf("\nFinal BBO:\n");
    if (b.has_bid) std::printf("Bid: %.2f x %ld\n", b.bid_price, b.bid_qty);
    else std::printf("Bid: none\n");
    if (b.has_ask) std::printf("Ask: %.2f x %ld\n", b.ask_price, b.ask_qty);
    else std::printf("Ask: none\n");
    std::printf("Live orders: %zu\n", book->order_count());
    std::printf("\nChecksum: %llu\n", (unsigned long long)checksum);
    return 0;
}

// ---------------------------------------------------------------- verify

// Dumb reference book: qty per tick in a plain vector and a full scan for
// every query. Slow, but simple enough that we trust it.
class NaiveBook {
public:
    NaiveBook() : qty_(MAX_ASK_TICK + 1, 0) {}

    bool add_order(uint64_t id, Side side, double price, long qty) {
        int tick;
        if (!valid_qty(qty) || !price_to_tick(side, price, tick)) return false;
        if (orders_.count(id)) return false;
        orders_[id] = {tick, qty};
        qty_[tick] += qty;
        return true;
    }

    bool cancel_order(uint64_t id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return false;
        qty_[it->second.first] -= it->second.second;
        orders_.erase(it);
        return true;
    }

    BBO get_bbo() const {
        BBO b;
        auto bids = get_bids(1), asks = get_asks(1);
        if (!bids.empty()) { b.has_bid = true; b.bid_price = bids[0].price; b.bid_qty = bids[0].qty; }
        if (!asks.empty()) { b.has_ask = true; b.ask_price = asks[0].price; b.ask_qty = asks[0].qty; }
        return b;
    }

    std::vector<Level> get_bids(int depth = 10) const {
        std::vector<Level> out;
        for (int t = MAX_BID_TICK; t >= MIN_BID_TICK && (int)out.size() < depth; t--)
            if (qty_[t] > 0) out.push_back({to_price(t), qty_[t]});
        return out;
    }

    std::vector<Level> get_asks(int depth = 10) const {
        std::vector<Level> out;
        for (int t = MIN_ASK_TICK; t <= MAX_ASK_TICK && (int)out.size() < depth; t++)
            if (qty_[t] > 0) out.push_back({to_price(t), qty_[t]});
        return out;
    }

private:
    std::vector<long> qty_;
    std::unordered_map<uint64_t, std::pair<int, long>> orders_;
};

bool same_bbo(const BBO& a, const BBO& b) {
    if (a.has_bid != b.has_bid || a.has_ask != b.has_ask) return false;
    if (a.has_bid && (to_tick(a.bid_price) != to_tick(b.bid_price) || a.bid_qty != b.bid_qty)) return false;
    if (a.has_ask && (to_tick(a.ask_price) != to_tick(b.ask_price) || a.ask_qty != b.ask_qty)) return false;
    return true;
}

bool same_levels(const std::vector<Level>& a, const std::vector<Level>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (to_tick(a[i].price) != to_tick(b[i].price) || a[i].qty != b[i].qty) return false;
    return true;
}

// small fixed scenarios from the assignment's "required behavior" section
template <class Book>
bool edge_cases(std::string& why) {
#define CHECK(cond)                       \
    do {                                  \
        if (!(cond)) {                    \
            why = "failed: " #cond;       \
            return false;                 \
        }                                 \
    } while (0)

    auto bk = std::make_unique<Book>();
    Book& book = *bk;

    // empty book
    BBO b = book.get_bbo();
    CHECK(!b.has_bid && !b.has_ask);
    CHECK(book.get_bids(10).empty() && book.get_asks(10).empty());

    // cancel something that never existed
    CHECK(!book.cancel_order(12345));

    // bad prices / qty get rejected
    CHECK(!book.add_order(1, Side::Buy, 100.00, 10));   // mid, not in bid band
    CHECK(!book.add_order(1, Side::Buy, 49.99, 10));
    CHECK(!book.add_order(1, Side::Sell, 99.99, 10));   // ask below band
    CHECK(!book.add_order(1, Side::Sell, 150.01, 10));
    CHECK(!book.add_order(1, Side::Buy, 99.985, 10));   // not on the tick grid
    CHECK(!book.add_order(1, Side::Buy, 99.00, 0));
    CHECK(!book.add_order(1, Side::Buy, 99.00, -5));
    b = book.get_bbo();
    CHECK(!b.has_bid && !b.has_ask);

    // three bid levels, two orders on one of them
    CHECK(book.add_order(1, Side::Buy, 99.50, 10));
    CHECK(book.add_order(2, Side::Buy, 99.98, 20));
    CHECK(book.add_order(3, Side::Buy, 99.50, 5));
    CHECK(book.add_order(4, Side::Buy, 50.00, 7));
    CHECK(!book.add_order(4, Side::Buy, 60.00, 7));  // duplicate id

    auto bids = book.get_bids(10);  // only 3 levels exist
    CHECK(bids.size() == 3);
    CHECK(to_tick(bids[0].price) == 9998 && bids[0].qty == 20);
    CHECK(to_tick(bids[1].price) == 9950 && bids[1].qty == 15);
    CHECK(to_tick(bids[2].price) == 5000 && bids[2].qty == 7);
    CHECK(book.get_bids(2).size() == 2);
    CHECK(book.get_bids(0).empty());

    b = book.get_bbo();
    CHECK(b.has_bid && !b.has_ask && to_tick(b.bid_price) == 9998 && b.bid_qty == 20);

    // asks
    CHECK(book.add_order(10, Side::Sell, 100.01, 3));
    CHECK(book.add_order(11, Side::Sell, 150.00, 4));
    CHECK(book.add_order(12, Side::Sell, 100.01, 6));
    auto asks = book.get_asks(10);
    CHECK(asks.size() == 2);
    CHECK(to_tick(asks[0].price) == 10001 && asks[0].qty == 9);
    CHECK(to_tick(asks[1].price) == 15000 && asks[1].qty == 4);

    // cancel the best bid -> next level becomes best
    CHECK(book.cancel_order(2));
    CHECK(!book.cancel_order(2));  // second cancel of same id
    b = book.get_bbo();
    CHECK(to_tick(b.bid_price) == 9950 && b.bid_qty == 15);

    // partial level cancel keeps the level
    CHECK(book.cancel_order(1));
    b = book.get_bbo();
    CHECK(to_tick(b.bid_price) == 9950 && b.bid_qty == 5);

    // cancel best ask orders one by one
    CHECK(book.cancel_order(10));
    CHECK(book.cancel_order(12));
    b = book.get_bbo();
    CHECK(b.has_ask && to_tick(b.ask_price) == 15000 && b.ask_qty == 4);

    // empty everything
    CHECK(book.cancel_order(3));
    CHECK(book.cancel_order(4));
    CHECK(book.cancel_order(11));
    b = book.get_bbo();
    CHECK(!b.has_bid && !b.has_ask);
    CHECK(book.get_bids(10).empty() && book.get_asks(10).empty());

    // book still works after being emptied
    CHECK(book.add_order(100, Side::Sell, 120.00, 1));
    b = book.get_bbo();
    CHECK(b.has_ask && to_tick(b.ask_price) == 12000 && b.ask_qty == 1);
#undef CHECK
    return true;
}

// Runs the same random workload through the naive book and one real book and
// compares after every op: cancel return value, BBO, top 10 bids and asks.
// Every 1000 ops it also compares the full depth.
template <class Book>
bool random_check(const std::vector<Op>& ops, std::string& why) {
    NaiveBook ref;
    auto book = std::make_unique<Book>();
    char buf[200];

    for (size_t i = 0; i < ops.size(); i++) {
        const Op& op = ops[i];
        if (op.type == OpType::Add) {
            bool a = ref.add_order(op.id, op.side, op.price, op.qty);
            bool b = book->add_order(op.id, op.side, op.price, op.qty);
            if (a != b) { std::snprintf(buf, sizeof buf, "op %zu: add returned %d, expected %d", i, b, a); why = buf; return false; }
        } else if (op.type == OpType::Cancel) {
            bool a = ref.cancel_order(op.id);
            bool b = book->cancel_order(op.id);
            if (a != b) { std::snprintf(buf, sizeof buf, "op %zu: cancel(%llu) returned %d, expected %d", i, (unsigned long long)op.id, b, a); why = buf; return false; }
        }

        if (!same_bbo(ref.get_bbo(), book->get_bbo())) { std::snprintf(buf, sizeof buf, "op %zu: BBO mismatch", i); why = buf; return false; }
        if (!same_levels(ref.get_bids(10), book->get_bids(10))) { std::snprintf(buf, sizeof buf, "op %zu: bid levels mismatch", i); why = buf; return false; }
        if (!same_levels(ref.get_asks(10), book->get_asks(10))) { std::snprintf(buf, sizeof buf, "op %zu: ask levels mismatch", i); why = buf; return false; }

        if (i % 1000 == 999) {
            if (!same_levels(ref.get_bids(100000), book->get_bids(100000)) ||
                !same_levels(ref.get_asks(100000), book->get_asks(100000))) {
                std::snprintf(buf, sizeof buf, "op %zu: full depth mismatch", i); why = buf; return false;
            }
        }
    }
    return true;
}

// same workload through A, B and C side by side; also checks the bench
// checksum comes out the same for all three
uint64_t bench_checksum_A(const std::vector<Op>& ops) { return run_once<MapBook>(ops).checksum; }
uint64_t bench_checksum_B(const std::vector<Op>& ops) { return run_once<HeapBook>(ops).checksum; }
uint64_t bench_checksum_C(const std::vector<Op>& ops) { return run_once<ArrayBook>(ops).checksum; }

int run_verify(size_t n_ops, uint32_t seed) {
    std::printf("Running correctness test...\n\n");
    // normal mix (book keeps growing) + a thin book where adds ~= cancels,
    // so levels keep emptying and the best price moves around a lot
    std::vector<Op> ops = make_workload(n_ops, seed, 5);
    std::vector<Op> thin = make_workload(n_ops, seed + 1, 5, 45);
    WorkloadCounts c = count_ops(ops), t = count_ops(thin);
    std::printf("1. edge cases (empty book, unknown/double cancel, bad prices, short depth, ...)\n");
    std::printf("2. random workload: %zu ops (add %zu, cancel %zu, bbo %zu), seed %u\n", ops.size(), c.add,
                c.cancel, c.bbo, seed);
    std::printf("3. thin book workload: %zu ops (add %zu, cancel %zu, bbo %zu), seed %u\n", thin.size(),
                t.add, t.cancel, t.bbo, seed + 1);
    std::printf("in 2 and 3 every op is compared against a brute force reference book\n\n");

    bool all_ok = true;
    auto check = [&](const char* name, bool (*edge)(std::string&),
                     bool (*rnd)(const std::vector<Op>&, std::string&)) {
        std::string why;
        bool ok = edge(why);
        if (ok) ok = rnd(ops, why);
        if (ok) ok = rnd(thin, why);
        std::printf("Implementation %s: %s", name, ok ? "PASS" : "FAIL");
        if (!ok) std::printf("  (%s)", why.c_str());
        std::printf("\n");
        all_ok = all_ok && ok;
    };
    check("A", edge_cases<MapBook>, random_check<MapBook>);
    check("B", edge_cases<HeapBook>, random_check<HeapBook>);
    check("C", edge_cases<ArrayBook>, random_check<ArrayBook>);

    // and the actual benchmark workload: all three must end with the same checksum
    std::vector<Op> bench_ops = make_workload(1000000, 42);
    uint64_t a = bench_checksum_A(bench_ops), b = bench_checksum_B(bench_ops), cc = bench_checksum_C(bench_ops);
    std::printf("\nbenchmark workload checksum: A=%llu B=%llu C=%llu %s\n", (unsigned long long)a,
                (unsigned long long)b, (unsigned long long)cc, (a == b && b == cc) ? "(match)" : "(MISMATCH)");
    all_ok = all_ok && a == b && b == cc;

    if (all_ok)
        std::printf("\nAll implementations produce identical results.\n");
    else
        std::printf("\nImplementations DISAGREE.\n");
    return all_ok ? 0 : 1;
}

// ---------------------------------------------------------------- main

void usage() {
    std::printf("usage: ./bench --impl A|B|C [--runs N] [--ops N] [--seed S] [--no-timers]\n");
    std::printf("       ./bench --verify [--ops N] [--seed S]\n");
}

int main(int argc, char** argv) {
    std::string impl;
    bool verify = false;
    int runs = 5;
    size_t ops = 0;
    uint32_t seed = 0;
    bool seed_set = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--verify") verify = true;
        else if (a == "--no-timers") g_op_timers = false;
        else if (a == "--impl" && i + 1 < argc) impl = argv[++i];
        else if (a == "--runs" && i + 1 < argc) runs = std::atoi(argv[++i]);
        else if (a == "--ops" && i + 1 < argc) ops = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--seed" && i + 1 < argc) { seed = std::strtoul(argv[++i], nullptr, 10); seed_set = true; }
        else { usage(); return 1; }
    }

    if (verify) return run_verify(ops ? ops : 200000, seed_set ? seed : 7);

    if (ops == 0) ops = 1000000;
    if (!seed_set) seed = 42;
    if (runs < 1) runs = 1;
    if (impl == "A") return run_bench<MapBook>("A (std::map + unordered_map)", ops, seed, runs);
    if (impl == "B") return run_bench<HeapBook>("B (priority_queue + lazy delete)", ops, seed, runs);
    if (impl == "C") return run_bench<ArrayBook>("C (flat array + bitmap + own hash table)", ops, seed, runs);
    usage();
    return 1;
}
