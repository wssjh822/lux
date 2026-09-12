#!/usr/bin/env bash
# Lux 0.5 回归测试
#   tests/cases/*.lux  行为测试：编译运行，stdout 与 *.expected 逐行比对
#   tests/errors/*.lux 诊断测试：编译必须失败，stderr 需包含 *.err 中的关键字
#   另含包管理与 REPL 冒烟测试
set -u
cd "$(dirname "$0")/.."
LUXC=build/luxc
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# 包安装目录隔离到本次测试的临时目录（luxHome() 尊重 LUX_HOME）
export LUX_HOME="$TMP/luxhome"

pass=0 fail=0 failed_cases=()

ok()   { pass=$((pass+1)); printf '  \033[32mok\033[0m   %s\n' "$1"; }
bad()  { fail=$((fail+1)); failed_cases+=("$1"); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }

echo "== 行为测试（tests/cases）=="
for src in tests/cases/*.lux; do
    name=$(basename "$src" .lux)
    exp="tests/cases/$name.expected"
    [ -f "$exp" ] || { bad "$name (缺少 .expected)"; continue; }
    # 在 cases 目录下编译运行（相对路径 import / 包夹具依赖 cwd）。
    # 编译与运行分两阶段：--run 会把运行期 stderr（panic 等）混进编译输出，
    # 运行时 panic 是被测行为而不是编译失败（0.5.1 修复误判）。
    bin="cases_bin_$name"
    cerr=$(cd tests/cases && "$OLDPWD/$LUXC" "$name.lux" -o "$TMP/$bin" 2>&1)
    if [ -n "$cerr" ]; then
        bad "$name (编译输出: $cerr)"; continue
    fi
    runout=$(cd tests/cases && "$TMP/$bin" 2>&1)
    if diff -u "$exp" <(printf '%s\n' "$runout") > "$TMP/$name.diff" 2>&1; then
        ok "$name"
    else
        bad "$name"; sed 's/^/    /' "$TMP/$name.diff" | head -20
    fi
done

echo "== 诊断测试（tests/errors）=="
if ls tests/errors/*.lux >/dev/null 2>&1; then
    for src in tests/errors/*.lux; do
        name=$(basename "$src" .lux)
        # .err = 编译必须失败且 stderr 含关键字；.warn = 编译必须成功且输出警告关键字
        if [ -f "tests/errors/$name.err" ]; then
            mode=err; kw=$(cat "tests/errors/$name.err")
        elif [ -f "tests/errors/$name.warn" ]; then
            mode=warn; kw=$(cat "tests/errors/$name.warn")
        else
            bad "$name (缺少 .err / .warn)"; continue
        fi
        out=$("$LUXC" "$src" -o "$TMP/diag_bin" 2>&1 >/dev/null)
        if [ "$mode" = err ]; then
            if printf '%s' "$out" | grep -qF "$kw"; then
                ok "$name"
            else
                bad "$name (错误信息中未找到关键字 '$kw')"
                printf '%s\n' "$out" | sed 's/^/    /' | head -8
            fi
        else
            if printf '%s' "$out" | grep -qF "$kw"; then
                ok "$name"
            else
                bad "$name (输出中未找到警告关键字 '$kw')"
                printf '%s\n' "$out" | sed 's/^/    /' | head -8
            fi
        fi
    done
else
    echo "  （无诊断用例）"
fi

echo "== REPL 冒烟 =="
repl_out=$(printf 'let x = 40;\nlet y = 2;\nx * y\n1 + "x"\n' | "$LUXC" repl 2>&1)
if printf '%s' "$repl_out" | grep -q '80' && printf '%s' "$repl_out" | grep -qi '错误\|error'; then
    ok "repl（求值 + 错误诊断）"
else
    bad "repl"; printf '%s\n' "$repl_out" | sed 's/^/    /' | head -10
fi

echo "== 原生后端冒烟（--native）=="
# 0.6 新增：对行为用例逐个跑原生后端（本机架构 ELF 直出），与同一 .expected 比对。
# extern fn 是 C 后端专属（libc 绑定），原生后端会定向报错——这类用例跳过。
native_skip=0
for src in tests/cases/*.lux; do
    name=$(basename "$src" .lux)
    exp="tests/cases/$name.expected"
    [ -f "$exp" ] || continue
    # --native 成功时也会打印提示行，所以用退出码而非输出判失败
    if "$LUXC" --native "$src" -o "$TMP/nat_$name" >"$TMP/nat_$name.log" 2>&1; then
        runout=$("$TMP/nat_$name" 2>&1)
        if diff -u "$exp" <(printf '%s\n' "$runout") > "$TMP/nat_$name.diff" 2>&1; then
            ok "native $name"
        else
            bad "native $name"; sed 's/^/    /' "$TMP/nat_$name.diff" | head -20
        fi
        # 差分对拍：同一用例在 C 后端与原生后端下输出必须逐字节一致
        # （两套独立的代码生成路径互相校验，是性价比最高的正确性保障）
        if [ -x "$TMP/cases_bin_$name" ]; then
            cout=$(cd tests/cases && "$TMP/cases_bin_$name" 2>&1)
            if diff -q <(printf '%s\n' "$runout") <(printf '%s\n' "$cout") >/dev/null; then
                ok "差分 $name（C == native）"
            else
                bad "差分 $name（C 与 native 输出不一致）"
                diff -u <(printf '%s\n' "$cout") <(printf '%s\n' "$runout") \
                    | sed 's/^/    /' | head -20
            fi
        fi
    else
        if grep -q '原生后端不支持' "$TMP/nat_$name.log" 2>/dev/null; then
            native_skip=$((native_skip+1))
        else
            bad "native $name (编译失败: $(head -3 "$TMP/nat_$name.log"))"
        fi
    fi
done

echo "== 包管理 =="
mkdir -p "$TMP/mypkg"
cat > "$TMP/mypkg/lib.lux" <<'EOF'
const TAG = "pkg-ok";
fn twice(n: int) -> int { return n * 2; }
EOF
cat > "$TMP/mypkg/lux.json" <<'EOF'
{ "name": "mypkg", "main": "lib.lux" }
EOF
cat > "$TMP/usepkg.lux" <<'EOF'
import "mypkg";
fn main() {
    println(TAG);
    println(twice(21));
}
EOF
if (cd "$TMP" && "$OLDPWD/$LUXC" add ./mypkg >/dev/null 2>&1) && \
   (cd "$TMP" && "$OLDPWD/$LUXC" usepkg.lux --run >"$TMP/pkg.out" 2>&1) && \
   grep -q 'pkg-ok' "$TMP/pkg.out" && grep -q '42' "$TMP/pkg.out"; then
    ok "pkg（add + import + 运行）"
else
    bad "pkg"; cat "$TMP/pkg.out" 2>/dev/null | sed 's/^/    /' | head -8
fi
# add 重复安装应报错（幂等性检查）
if (cd "$TMP" && "$OLDPWD/$LUXC" add ./mypkg >/dev/null 2>&1); then
    bad "pkg 重复 add（应报错）"
else
    ok "pkg 重复 add（应报错）"
fi
(cd "$TMP" && "$OLDPWD/$LUXC" delete mypkg >/dev/null 2>&1) && ok "pkg delete" \
    || bad "pkg delete"

echo
if [ "$fail" -eq 0 ]; then
    printf '\033[32m全部 %d 项测试通过\033[0m\n' "$pass"
    exit 0
else
    printf '\033[31m%d 项通过，%d 项失败\033[0m：' "$pass" "$fail"
    printf ' %s' "${failed_cases[@]}"
    printf '\n'
    exit 1
fi
