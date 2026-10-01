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
    # 运行参数（可选）：tests/cases/<name>.args，按空白拆分
    case_args=()
    if [ -f "tests/cases/$name.args" ]; then
        read -r -a case_args < "tests/cases/$name.args"
    fi
    runout=$(cd tests/cases && "$TMP/$bin" "${case_args[@]}" 2>&1)
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
        nat_args=()
        if [ -f "tests/cases/$name.args" ]; then
            read -r -a nat_args < "tests/cases/$name.args"
        fi
        runout=$("$TMP/nat_$name" "${nat_args[@]}" 2>&1)
        if diff -u "$exp" <(printf '%s\n' "$runout") > "$TMP/nat_$name.diff" 2>&1; then
            ok "native $name"
        else
            bad "native $name"; sed 's/^/    /' "$TMP/nat_$name.diff" | head -20
        fi
        # 差分对拍：同一用例在 C 后端与原生后端下输出必须逐字节一致
        # （两套独立的代码生成路径互相校验，是性价比最高的正确性保障）
        if [ -x "$TMP/cases_bin_$name" ]; then
            cout=$(cd tests/cases && "$TMP/cases_bin_$name" "${nat_args[@]}" 2>&1)
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

echo "== ARC（--arc，C 后端引用计数，0.9.2 实验特性） =="
# 对全部行为用例再跑一遍 --arc：输出必须与非 ARC 完全一致，
# 并用 MALLOC_CHECK_ / MALLOC_PERTURB_ 抓双重释放与使用已释放内存。
arc_fails=0
for src in tests/cases/*.lux; do
    name=$(basename "$src" .lux)
    exp="tests/cases/$name.expected"
    [ -f "$exp" ] || continue
    cerr=$(cd tests/cases && "$OLDPWD/$LUXC" --arc "$name.lux" -o "$TMP/arc_$name" 2>&1)
    if [ -n "$cerr" ]; then
        bad "arc $name（编译: $(printf '%s' "$cerr" | head -1)）"; arc_fails=$((arc_fails+1)); continue
    fi
    aargs=()
    [ -f "tests/cases/$name.args" ] && read -r -a aargs < "tests/cases/$name.args"
    arcout=$(cd tests/cases && MALLOC_CHECK_=3 MALLOC_PERTURB_=173 "$TMP/arc_$name" "${aargs[@]}" 2>&1)
    if ! diff -q "$exp" <(printf '%s\n' "$arcout") >/dev/null; then
        bad "arc $name（输出不一致）"; arc_fails=$((arc_fails+1))
    fi
done
[ "$arc_fails" -eq 0 ] && ok "arc（全部行为用例：输出一致 + 无堆损坏）"
# --arc + --native 必须明确拒绝（原生后端 ARC 计划 0.9.3）
if "$LUXC" --native --arc tests/cases/arc_stress.lux -o "$TMP/arc_reject" 2>&1 | grep -q '仅支持 C 后端'; then
    ok "arc（--native 明确拒绝）"
else
    bad "arc（--native 应拒绝）"
fi
# 内存回归：ARC 下 churn 循环必须保持常驻内存（非 ARC 会涨到上百 MB）
cat > "$TMP/arc_churn.lux" <<'EOF'
fn main() {
    let i = 0;
    while i < 1000000 {
        let s = "a" + string(i) + "b";
        if len(s) < 0 { println(s); }
        i += 1;
    }
    println("done");
}
EOF
if "$LUXC" --arc "$TMP/arc_churn.lux" -o "$TMP/arc_churn" >/dev/null 2>&1; then
    churn_ok=0; maxrss=0
    if [ -r /proc/self/status ]; then
        "$TMP/arc_churn" >/dev/null 2>&1 &
        cpid=$!
        while kill -0 "$cpid" 2>/dev/null; do
            r=$(awk '/VmHWM/{print $2}' "/proc/$cpid/status" 2>/dev/null)
            if [ -n "$r" ] && [ "$r" -gt "$maxrss" ]; then maxrss=$r; fi
            sleep 0.05
        done
        wait "$cpid"
        # 1e6 次字符串构造在 ARC 下应远低于 64 MiB（非 ARC 会到 ~117 MiB）
        if [ "$maxrss" -gt 0 ] && [ "$maxrss" -lt 65536 ]; then churn_ok=1; fi
    else
        churn_ok=1  # 非 Linux：跳过内存断言
    fi
    if [ "$churn_ok" -eq 1 ]; then ok "arc（churn 循环内存保持常驻）"
    else bad "arc（churn 峰值内存过高: ${maxrss}kB）"; fi
else
    bad "arc（churn 编译失败）"
fi

echo "== luxc build（LuxBuildFile 项目构建） =="
BPROJ="$TMP/bproj"
mkdir -p "$BPROJ/src" "$BPROJ/tests"
cat > "$BPROJ/src/main.lux" <<'EOF'
fn main() { println("built-ok"); }
EOF
cat > "$BPROJ/tests/one.lux" <<'EOF'
fn main() { println(6 * 7); }
EOF
cat > "$BPROJ/tests/one.expected" <<'EOF'
42
EOF
cat > "$BPROJ/LuxBuildFile" <<'EOF'
name    = bproj
main    = src/main.lux
out     = build/bproj
backend = c
opt     = 2
testdir = tests
EOF
if "$LUXC" build "$BPROJ" >"$TMP/build.log" 2>&1 && [ -x "$BPROJ/build/bproj" ] && \
   [ "$("$BPROJ/build/bproj")" = "built-ok" ]; then
    ok "build（构建 + 运行产物）"
else
    bad "build"; sed 's/^/    /' "$TMP/build.log" | head -10
fi
if "$LUXC" build "$BPROJ" test >"$TMP/build_test.log" 2>&1; then
    ok "build test（跑测试并比对 .expected）"
else
    bad "build test"; sed 's/^/    /' "$TMP/build_test.log" | head -10
fi
if "$LUXC" build "$BPROJ" clean >/dev/null 2>&1 && [ ! -e "$BPROJ/build/bproj" ]; then
    ok "build clean"
else
    bad "build clean"
fi

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

echo "== 官方包 jsonx（JSON 解析 / 生成） =="
if (cd "$TMP" && "$OLDPWD/$LUXC" add "$OLDPWD/packages/jsonx" >/dev/null 2>&1); then
    if (cd "$OLDPWD" && "$LUXC" tests/pkgs/jsonx_test.lux -o "$TMP/jsonx_app") >/dev/null 2>&1 && \
       diff -u "$OLDPWD/tests/pkgs/jsonx_test.expected" <("$TMP/jsonx_app" 2>&1) >/dev/null; then
        ok "jsonx（解析 + 取值 + 生成）"
    else
        bad "jsonx（行为不符）"
        (cd "$OLDPWD" && "$LUXC" tests/pkgs/jsonx_test.lux -o "$TMP/jsonx_app" 2>&1; "$TMP/jsonx_app" 2>&1) | sed 's/^/    /' | head -10
    fi
    if (cd "$OLDPWD" && "$LUXC" --native tests/pkgs/jsonx_test.lux -o "$TMP/jsonx_appn") >/dev/null 2>&1 && \
       diff -u "$OLDPWD/tests/pkgs/jsonx_test.expected" <("$TMP/jsonx_appn" 2>&1) >/dev/null; then
        ok "jsonx（原生后端差分）"
    else
        bad "jsonx（原生后端输出不一致）"
    fi
    (cd "$TMP" && "$OLDPWD/$LUXC" delete jsonx >/dev/null 2>&1)
else
    bad "jsonx（安装失败）"
fi

echo "== 官方包 mathx / strx / arrx（自带库） =="
if (cd "$TMP" && "$OLDPWD/$LUXC" add "$OLDPWD/packages/mathx" >/dev/null 2>&1) && \
   (cd "$TMP" && "$OLDPWD/$LUXC" add "$OLDPWD/packages/strx" >/dev/null 2>&1) && \
   (cd "$TMP" && "$OLDPWD/$LUXC" add "$OLDPWD/packages/arrx" >/dev/null 2>&1); then
    if (cd "$OLDPWD" && "$LUXC" tests/pkgs/xpkgs_test.lux -o "$TMP/xpkgs_app") >/dev/null 2>&1 && \
       diff -u "$OLDPWD/tests/pkgs/xpkgs_test.expected" <("$TMP/xpkgs_app" 2>&1) >/dev/null; then
        ok "mathx / strx / arrx（行为）"
    else
        bad "mathx / strx / arrx（行为不符）"
        (cd "$OLDPWD" && "$LUXC" tests/pkgs/xpkgs_test.lux -o "$TMP/xpkgs_app" 2>&1; "$TMP/xpkgs_app" 2>&1) | sed 's/^/    /' | head -10
    fi
    if (cd "$OLDPWD" && "$LUXC" --native tests/pkgs/xpkgs_test.lux -o "$TMP/xpkgs_appn") >/dev/null 2>&1 && \
       diff -u "$OLDPWD/tests/pkgs/xpkgs_test.expected" <("$TMP/xpkgs_appn" 2>&1) >/dev/null; then
        ok "mathx / strx / arrx（原生后端差分）"
    else
        bad "mathx / strx / arrx（原生后端输出不一致）"
    fi
    (cd "$TMP" && "$OLDPWD/$LUXC" delete mathx >/dev/null 2>&1)
    (cd "$TMP" && "$OLDPWD/$LUXC" delete strx >/dev/null 2>&1)
    (cd "$TMP" && "$OLDPWD/$LUXC" delete arrx >/dev/null 2>&1)
else
    bad "mathx / strx / arrx（安装失败）"
fi

echo "== 在线包管理（本地 file:// 注册表） =="
# 用一个本地的 file:// 索引把「下载 -> 校验 -> 依赖 -> 安装 -> import」整条链路
# 跑一遍，不依赖网络。sha256 用系统工具算；两者都没有就跳过。
SHA=""
if command -v sha256sum >/dev/null 2>&1; then
    SHA="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
    SHA="shasum -a 256"
fi
if [ -n "$SHA" ] && command -v tar >/dev/null 2>&1; then
    REG="$TMP/registry"
    mkdir -p "$REG"
    # 两个测试包：tuse 依赖 tmath，用来验证依赖自动安装
    mkdir -p "$TMP/regsrc/tmath" "$TMP/regsrc/tuse"
    cat > "$TMP/regsrc/tmath/lib.lux" <<'EOF'
fn triple(n: int) -> int { return n * 3; }
EOF
    cat > "$TMP/regsrc/tmath/lux.json" <<'EOF'
{ "name": "tmath", "version": "1.0.0", "main": "lib.lux", "files": ["lib.lux"] }
EOF
    cat > "$TMP/regsrc/tuse/lib.lux" <<'EOF'
import "tmath";
fn nine(n: int) -> int { return triple(triple(n)); }
EOF
    cat > "$TMP/regsrc/tuse/lux.json" <<'EOF'
{ "name": "tuse", "version": "1.0.0", "main": "lib.lux", "files": ["lib.lux"], "deps": { "tmath": "^1.0.0" } }
EOF
    for p in tmath tuse; do
        tar -czf "$REG/$p-1.0.0.tar.gz" -C "$TMP/regsrc/$p" .
    done
    S1=$($SHA "$REG/tmath-1.0.0.tar.gz" | awk '{print $1}')
    S2=$($SHA "$REG/tuse-1.0.0.tar.gz" | awk '{print $1}')
    cat > "$REG/index.json" <<EOF
{
  "schema": 1,
  "packages": [
    { "name": "tmath", "version": "1.0.0", "main": "lib.lux",
      "files": ["lib.lux"], "url": "file://$REG/tmath-1.0.0.tar.gz",
      "sha256": "$S1" },
    { "name": "tuse", "version": "1.0.0", "main": "lib.lux",
      "files": ["lib.lux"], "deps": { "tmath": "^1.0.0" },
      "url": "file://$REG/tuse-1.0.0.tar.gz", "sha256": "$S2" }
  ]
}
EOF
    export LUX_REGISTRY="file://$REG/index.json"
    if "$LUXC" update >/dev/null 2>&1 && \
       "$LUXC" install tuse >"$TMP/reg_install.log" 2>&1 && \
       [ -f "$LUX_HOME/packages/tmath/lux.json" ] && \
       [ -f "$LUX_HOME/packages/tuse/lux.json" ]; then
        ok "registry（下载 + 校验 + 依赖安装）"
        cat > "$TMP/reg_app.lux" <<'EOF'
import "tuse";
fn main() { println(nine(4)); }
EOF
        if (cd "$TMP" && "$OLDPWD/$LUXC" reg_app.lux --run >"$TMP/reg_app.out" 2>&1) && \
           grep -q '36' "$TMP/reg_app.out"; then
            ok "registry（安装后 import + 运行）"
        else
            bad "registry（import 运行失败）"
            sed 's/^/    /' "$TMP/reg_app.out" | head -8
        fi
        # 再装一次应幂等成功（同版本）
        if "$LUXC" install tuse >/dev/null 2>&1; then
            ok "registry（重复安装同版本幂等）"
        else
            bad "registry（重复安装同版本应成功）"
        fi
    else
        bad "registry（安装链路失败）"
        sed 's/^/    /' "$TMP/reg_install.log" 2>/dev/null | head -10
    fi
    unset LUX_REGISTRY
else
    echo "  （缺少 sha256sum/tar，跳过在线包管理测试）"
fi

echo "== 注册表账号 / 上传删除（PHP 内置服务器） =="
# 有 php 时，把服务端的注册→登录→上传→权限→删除整条链路跑一遍。
if command -v php >/dev/null 2>&1; then
    SRV="$TMP/regsrv"
    mkdir -p "$SRV"
    # 故意放一个旧版数据文件，验证 0.9.3 的 .php 守卫自动迁移
    mkdir -p "$SRV/data"
    echo '{}' > "$SRV/data/users.json"
    cp server/lux.php server/lib.php server/index.php "$SRV/"
    PORT=$(( 20000 + RANDOM % 20000 ))
    php -S "127.0.0.1:$PORT" -t "$SRV" >"$TMP/regsrv.log" 2>&1 &
    SRV_PID=$!
    trap 'kill $SRV_PID 2>/dev/null; rm -rf "$TMP"' EXIT
    B="http://127.0.0.1:$PORT"
    ready=0
    for _ in $(seq 1 40); do
        if curl -sf -m 1 "$B/lux.php?action=health" >/dev/null 2>&1; then ready=1; break; fi
        sleep 0.25
    done
    jsonf() { php -r '$j=json_decode(stream_get_contents(STDIN),true); echo $j[$argv[1]]??"";' "$1"; }
    if [ "$ready" != "1" ]; then
        bad "registry 服务器启动"
    else
        R1=$(curl -s -X POST -d 'username=usera&password=secret1' "$B/lux.php?action=register")
        TOKA=$(printf '%s' "$R1" | jsonf token)
        R2=$(curl -s -X POST -d 'username=userb&password=secret2' "$B/lux.php?action=register")
        TOKB=$(printf '%s' "$R2" | jsonf token)
        if [ -n "$TOKA" ] && [ -n "$TOKB" ]; then
            ok "registry 账号（注册返回令牌）"
        else
            bad "registry 账号（注册）"; printf '%s\n' "$R1" | sed 's/^/    /' | head -5
        fi
        # 数据文件守卫：旧 users.json 已迁移删除，users.php 以 <?php 开头
        if [ -f "$SRV/data/users.php" ] && [ ! -f "$SRV/data/users.json" ] && \
           [ "$(head -c 5 "$SRV/data/users.php")" = "<?php" ]; then
            ok "registry 数据文件守卫（迁移 .json -> .php）"
        else
            bad "registry 数据文件守卫"
        fi
        # 造一个包并上传
        mkdir -p "$TMP/srvpkg"
        printf 'fn ping() -> int { return 7; }\n' > "$TMP/srvpkg/lib.lux"
        tar -czf "$TMP/srvpkg.tar.gz" -C "$TMP/srvpkg" .
        META='{"name":"srvpkg","version":"0.1.0","summary":"s","main":"lib.lux","files":["lib.lux"]}'
        PUB=$(curl -s -X POST -H "Authorization: Bearer $TOKA" -F "meta=$META" \
              -F "archive=@$TMP/srvpkg.tar.gz" "$B/lux.php?action=publish")
        if printf '%s' "$PUB" | grep -q '"ok": true'; then
            ok "registry 上传（需登录）"
        else
            bad "registry 上传"; printf '%s\n' "$PUB" | sed 's/^/    /' | head -5
        fi
        # 未登录上传必须失败
        if curl -s -X POST -F "meta=$META" -F "archive=@$TMP/srvpkg.tar.gz" \
                "$B/lux.php?action=publish" | grep -q '需要登录'; then
            ok "registry 未登录上传被拒"
        else
            bad "registry 未登录上传应被拒"
        fi
        # 他人不能删除（403）
        CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Authorization: Bearer $TOKB" \
               -d 'name=srvpkg&version=0.1.0' "$B/lux.php?action=delete")
        [ "$CODE" = "403" ] && ok "registry 他人删除被拒（403）" || bad "registry 他人删除应为 403（得到 $CODE）"
        # 本人删除成功
        if curl -s -X POST -H "Authorization: Bearer $TOKA" \
                -d 'name=srvpkg&version=0.1.0' "$B/lux.php?action=delete" | grep -q '"ok": true'; then
            ok "registry 本人删除"
        else
            bad "registry 本人删除"
        fi
        # 文件方式上传：上传具体文件，服务端打包（验证补全 lux.json）
        mkdir -p "$TMP/filespkg"
        printf 'fn a() -> int { return 1; }\n' > "$TMP/filespkg/lib.lux"
        printf 'fn b() -> int { return 2; }\n' > "$TMP/filespkg/extra.lux"
        FMETA='{"name":"filespkg","version":"0.1.0","summary":"files","main":"lib.lux"}'
        PF=$(curl -s -X POST -H "Authorization: Bearer $TOKA" -F "meta=$FMETA" \
             -F "files[]=@$TMP/filespkg/lib.lux" \
             -F "files[]=@$TMP/filespkg/extra.lux" "$B/lux.php?action=publish")
        if printf '%s' "$PF" | grep -q '"ok": true' && \
           tar -tzf "$SRV/packages/filespkg/0.1.0.tar.gz" | grep -qx 'lux.json' && \
           tar -tzf "$SRV/packages/filespkg/0.1.0.tar.gz" | grep -qx 'lib.lux' && \
           tar -tzf "$SRV/packages/filespkg/0.1.0.tar.gz" | grep -qx 'extra.lux'; then
            ok "registry 文件上传（服务端打包）"
        else
            bad "registry 文件上传"; printf '%s\n' "$PF" | sed 's/^/    /' | head -5
        fi
        # 更新包（文件方式）：只传改动文件，在基础版本上叠加
        printf 'fn c() -> int { return 3; }\n' > "$TMP/filespkg/new.lux"
        UMETA='{"name":"filespkg","version":"0.2.0","main":"lib.lux"}'
        PU=$(curl -s -X POST -H "Authorization: Bearer $TOKA" -F "meta=$UMETA" \
             -F "base_version=0.1.0" -F "files[]=@$TMP/filespkg/new.lux" "$B/lux.php?action=publish")
        if printf '%s' "$PU" | grep -q '"update": true' && \
           tar -tzf "$SRV/packages/filespkg/0.2.0.tar.gz" | grep -qx 'lib.lux' && \
           tar -tzf "$SRV/packages/filespkg/0.2.0.tar.gz" | grep -qx 'extra.lux' && \
           tar -tzf "$SRV/packages/filespkg/0.2.0.tar.gz" | grep -qx 'new.lux'; then
            ok "registry 更新包（文件叠加基础版本）"
        else
            bad "registry 更新包（文件）"; printf '%s\n' "$PU" | sed 's/^/    /' | head -5
        fi
        # 更新包（压缩包方式）：只含改动文件的 .tar.gz
        mkdir -p "$TMP/upd"
        printf 'fn d() -> int { return 4; }\n' > "$TMP/upd/changed.lux"
        tar -czf "$TMP/upd.tar.gz" -C "$TMP/upd" .
        AMETA='{"name":"filespkg","version":"0.3.0","main":"lib.lux"}'
        PA=$(curl -s -X POST -H "Authorization: Bearer $TOKA" -F "meta=$AMETA" \
             -F "base_version=0.2.0" -F "archive=@$TMP/upd.tar.gz" "$B/lux.php?action=publish")
        if printf '%s' "$PA" | grep -q '"ok": true' && \
           tar -tzf "$SRV/packages/filespkg/0.3.0.tar.gz" | grep -qx 'new.lux' && \
           tar -tzf "$SRV/packages/filespkg/0.3.0.tar.gz" | grep -qx 'changed.lux'; then
            ok "registry 更新包（压缩包叠加）"
        else
            bad "registry 更新包（压缩包）"; printf '%s\n' "$PA" | sed 's/^/    /' | head -5
        fi
        # 意见反馈：API 免登录提交 + 落盘 + 网页可访问
        FB=$(curl -s -X POST -d 'type=建议&content=测试反馈内容' "$B/lux.php?action=feedback")
        if printf '%s' "$FB" | grep -q '"ok": true' && \
           grep -q '测试反馈内容' "$SRV/data/feedback.php" && \
           curl -sf "$B/index.php?p=feedback" | grep -q '提交反馈'; then
            ok "registry 意见反馈（API + 网页）"
        else
            bad "registry 意见反馈"; printf '%s\n' "$FB" | sed 's/^/    /' | head -5
        fi
        kill $SRV_PID 2>/dev/null || true
    fi
else
    echo "  （缺少 php，跳过注册表服务端测试）"
fi

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
