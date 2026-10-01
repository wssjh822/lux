#!/usr/bin/env bash
# 0.8（E2）：lexer / parser 黑盒 fuzzing 冒烟。
# 给编译器喂随机字节与随机变异过的合法程序，断言它既不崩溃（不带信号退出），
# 也不会挂死（每条都加超时）。默认 200 轮，CI 里用 LUX_FUZZ_ITERS 调整。
set -u
cd "$(dirname "$0")/.."
LUXC=build/luxc
[ -x "$LUXC" ] || { echo "先跑 make 生成 $LUXC"; exit 1; }

ITERS=${LUX_FUZZ_ITERS:-200}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 一个合法种子，用于字节级变异
cat > "$TMP/seed.lux" <<'EOF'
import "math";
struct Point { let x: int; let y: int; }
fn dist(p: Point) -> float { return sqrt(float(p.x * p.x + p.y * p.y)); }
fn main(argv: string[]) {
    let p = Point { x: 3, y: 4 };
    let s = "hi";
    for c in s { print(c); }
    println(dist(p) or 0.0);
    let xs = [1, 2, 3];
    for x in xs { if x > 1 { println(x); } }
}
EOF

crashes=0
for i in $(seq 1 "$ITERS"); do
    case $((i % 3)) in
      0)  # 纯随机字节
          head -c $((RANDOM % 400 + 1)) /dev/urandom > "$TMP/f.lux" ;;
      1)  # 随机 ASCII
          tr -dc '[:print:]\n\t' < /dev/urandom | head -c $((RANDOM % 400 + 1)) > "$TMP/f.lux" ;;
      2)  # 合法种子 + 随机字节变异
          cp "$TMP/seed.lux" "$TMP/f.lux"
          dd if=/dev/urandom of="$TMP/f.lux" bs=1 count=$((RANDOM % 8 + 1)) \
             seek=$((RANDOM % 200)) conv=notrunc status=none 2>/dev/null || true ;;
    esac
    timeout 10 "$LUXC" "$TMP/f.lux" --emit-c >/dev/null 2>"$TMP/err"
    rc=$?
    # 124 = 超时；>=128 = 被信号杀死（段错误 139 / 中止 134 等）
    if [ "$rc" -eq 124 ] || [ "$rc" -ge 128 ]; then
        crashes=$((crashes + 1))
        cp "$TMP/f.lux" "$TMP/crash_$i.lux"
        echo "  crash/hang (rc=$rc): $TMP/crash_$i.lux"
    fi
done

if [ "$crashes" -eq 0 ]; then
    echo "fuzz: $ITERS 轮无崩溃/挂死"
    exit 0
fi
echo "fuzz: $crashes 轮出现崩溃/挂死"
exit 1
