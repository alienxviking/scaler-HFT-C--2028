#!/usr/bin/env bash
# builds everything, runs --verify, the benchmark and perf for A, B, C
# and saves all the output in results/
set -e
cd "$(dirname "$0")"

make -s bench
mkdir -p results

{ lscpu | grep -E "Model name|^CPU\(s\)|Thread|cache"; uname -r; g++ --version | head -1; perf --version; } > results/machine.txt

./bench --verify | tee results/verify.txt

for impl in A B C; do
    ./bench --impl $impl | tee results/bench_$impl.txt
done

EVENTS=task-clock,cycles,instructions,branches,branch-misses,cache-misses,L1-dcache-load-misses,dTLB-load-misses,page-faults

for impl in A B C; do
    echo "perf stat $impl"
    perf stat -r 5 -e $EVENTS ./bench --impl $impl > /dev/null 2> results/perf_stat_$impl.txt
    cat results/perf_stat_$impl.txt

    # same thing without the per-op timing pass, so clock_gettime doesn't
    # dominate the counters
    perf stat -r 5 -e $EVENTS ./bench --impl $impl --no-timers > /dev/null 2> results/perf_stat_${impl}_no_timers.txt
    cat results/perf_stat_${impl}_no_timers.txt

    echo "perf record $impl"
    perf record -g -o perf.data ./bench --impl $impl > /dev/null 2>&1
    {
        echo "### self time (perf report --no-children)"
        perf report -i perf.data --stdio --no-children -g none --percent-limit 0.5 2>/dev/null | grep -v "^$"
        echo
        echo "### including children, with call graph (perf report --children)"
        perf report -i perf.data --stdio --children -g caller,0.5,callee --percent-limit 5 2>/dev/null | grep -v "^$" | head -150
    } > results/perf_report_$impl.txt
done
rm -f perf.data perf.data.old

echo "per run counters"
./per_run_counters.sh | tee results/perf_per_run.txt
