#!/usr/bin/env bash
# perf stat for the whole program also counts workload generation, the final
# replay and process startup, which are the same for A/B/C. To get counters
# for the book alone we run --runs 1 and --runs 5 (both --no-timers) and take
# (runs5 - runs1) / 4 = cost of one run (one pass over the 1M ops).
set -e
cd "$(dirname "$0")"
make -s bench
EVENTS=cycles,instructions,branches,branch-misses,cache-misses,L1-dcache-load-misses,dTLB-load-misses,page-faults

get() {  # impl runs -> "event value" lines
    perf stat -x, -r 5 -e $EVENTS ./bench --impl $1 --runs $2 --no-timers 2>&1 >/dev/null |
        awk -F, '{print $3, $1}'
}

printf "%-24s %14s %14s %14s\n" "per run (1M ops)" A B C
for impl in A B C; do
    get $impl 1 > /tmp/r1_$impl
    get $impl 5 > /tmp/r5_$impl
done
for ev in ${EVENTS//,/ }; do
    printf "%-24s" $ev
    for impl in A B C; do
        a=$(awk -v e=$ev '$1==e{print $2}' /tmp/r1_$impl)
        b=$(awk -v e=$ev '$1==e{print $2}' /tmp/r5_$impl)
        printf " %14.0f" $(echo "($b - $a) / 4" | bc -l)
    done
    echo
done
