#!/usr/bin/env bash
# =============================================================================
#  Lux / C / Python 性能对比
#    同一套试除法素数算法，统计 2..2,000,000 之间的素数
#  用法: ./bench/run_bench.sh
# =============================================================================
set -u
cd "$(dirname "$0")" || exit 1

LUXC=${LUXC:-../build/luxc}
RUNS=${RUNS:-3}

echo "== 构建 =="
"$LUXC" primes_bench.lux -O2 -o primes_lux || exit 1
"$LUXC" --native primes_bench.lux -o primes_lux_native || exit 1
gcc -O2 -o primes_c primes_hand.c || exit 1
echo "  完成"
echo

echo "== 正确性校验（各方结果应当一致）=="
printf "  Lux(C后端) : %s\n" "$(./primes_lux | head -1)"
printf "  Lux(原生)  : %s\n" "$(./primes_lux_native | head -1)"
printf "  C          : %s\n" "$(./primes_c)"
printf "  Python     : %s\n" "$(python3 primes_hand.py)"
echo

# 跑 $RUNS 次，返回其中最快的一次耗时（秒）。
# 用 date +%s%N 计时（GNU/Linux 与 macOS 都能用），不依赖 /usr/bin/time。
now_ns() {
    if date +%s%N > /dev/null 2>&1; then
        date +%s%N
    else
        # macOS 的 date 不支持 %N，退化为 %s（精度 1 秒，仍可用）
        printf '%s000000000\n' "$(date +%s)"
    fi
}

best_of() {
    local best=""
    for _ in $(seq 1 "$RUNS"); do
        local t0 t1 t
        t0=$(now_ns)
        "$@" > /dev/null
        t1=$(now_ns)
        t=$(awk "BEGIN{printf \"%.3f\", ($t1 - $t0) / 1000000000}")
        if [ -z "$best" ]; then
            best=$t
        else
            best=$(printf "%s\n%s\n" "$best" "$t" | sort -n | head -1)
        fi
    done
    echo "$best"
}

echo "== 性能对比（各跑 $RUNS 次，取最快）=="
T_LUX=$(best_of ./primes_lux)
T_NAT=$(best_of ./primes_lux_native)
T_C=$(best_of ./primes_c)
T_PY=$(best_of python3 primes_hand.py)

printf "  %-22s %s 秒\n" "Lux (luxc -O2)" "$T_LUX"
printf "  %-22s %s 秒\n" "Lux (--native)" "$T_NAT"
printf "  %-22s %s 秒\n" "C (gcc -O2)" "$T_C"
printf "  %-22s %s 秒\n" "Python 3" "$T_PY"

echo
echo "== 结论 =="
printf "  Lux(C后端) 相对手写 C : %s ×   （1.00 即与 C 同速）\n" \
    "$(awk "BEGIN{printf \"%.2f\", $T_LUX/$T_C}")"
printf "  Lux(原生)  相对手写 C : %s ×\n" \
    "$(awk "BEGIN{printf \"%.2f\", $T_NAT/$T_C}")"
printf "  Lux(原生)  相对 Python: %s × 快\n" \
    "$(awk "BEGIN{printf \"%.1f\", $T_PY/$T_NAT}")"

rm -f primes_lux primes_lux_native primes_c
