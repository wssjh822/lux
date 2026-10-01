#!/usr/bin/env bash
# =============================================================================
#  make_downloads.sh : 生成注册表「源码下载」所需的分发包与清单
#
#  用法：server/make_downloads.sh [输出目录]   默认 <仓库>/dist/downloads
#
#  产物：
#    lux-server-<版本>.tar.gz     包管理系统源码（lux.php / lib.php / index.php …）
#    lux-<版本>-src.tar.gz        当前 Lux 编译器源码
#    lux-<旧版本>.zip             从 /sdcard/code/lux/ 收集的历史版本
#    downloads.json               供 index.php 下载页读取的清单
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
VER="$(grep -m1 '^LUX_VERSION' "$ROOT/Makefile" | sed -E 's/.*([0-9]+\.[0-9]+\.[0-9]+).*/\1/')"
OUT="${1:-$ROOT/dist/downloads}"
HIST="${LUX_HISTORY_DIR:-/sdcard/code/lux}"

mkdir -p "$OUT"
rm -f "$OUT"/lux-*.tar.gz "$OUT"/lux-*.zip "$OUT/downloads.json"

echo "版本: $VER"
echo "输出: $OUT"

# ---- 1. 包管理系统源码 ----
tar -czf "$OUT/lux-server-$VER.tar.gz" -C "$ROOT/server" \
    lux.php lib.php index.php README.md make_downloads.sh deploy.sh
echo "  + lux-server-$VER.tar.gz"

# ---- 2. 当前 Lux 源码 ----
tar -czf "$OUT/lux-$VER-src.tar.gz" -C "$ROOT" \
    --exclude='./build' --exclude='./.git' --exclude='./dist' \
    --exclude='./server/data' --exclude='./examples/*.c' \
    Makefile README.md CHANGELOG.md LICENSE logo.svg \
    src docs examples tests packages server
echo "  + lux-$VER-src.tar.gz"

# ---- 3. 历史版本（如果本机存在） ----
if [ -d "$HIST" ]; then
    for z in "$HIST"/lux-*.zip; do
        [ -f "$z" ] || continue
        base="$(basename "$z")"
        # 当前版本的 zip 若存在就跳过（用源码 tar.gz）
        case "$base" in *"$VER"*) continue;; esac
        cp "$z" "$OUT/$base"
        echo "  + $base"
    done
fi

# ---- 4. 清单 ----
python3 - "$OUT" "$VER" <<'PY'
import hashlib, json, os, sys, zipfile
out, ver = sys.argv[1], sys.argv[2]
items = []
def add(fname, kind, version, title, desc):
    path = os.path.join(out, fname)
    if not os.path.isfile(path):
        return
    h = hashlib.sha256(open(path, 'rb').read()).hexdigest()
    items.append({
        "title": title, "kind": kind, "version": version,
        "file": fname, "desc": desc,
        "size": os.path.getsize(path), "sha256": h,
    })
add(f"lux-server-{ver}.tar.gz", "server", ver,
    f"包管理系统源码 {ver}", "注册表 API + 网页界面 + 部署说明（PHP，无数据库依赖）")
for z in sorted(os.listdir(out)):
    if not z.endswith(".zip"):
        continue
    v = z[len("lux-"):-len(".zip")]
    add(z, "lux", v, f"Lux 编译器源码 {v}", "历史版本完整源码（C++）")
add(f"lux-{ver}-src.tar.gz", "lux", ver,
    f"Lux 编译器源码 {ver}", "当前版本完整源码：C++ / 文档 / 示例 / 测试 / 包管理")
json.dump({"version": ver, "downloads": items}, open(os.path.join(out, "downloads.json"), "w"),
          ensure_ascii=False, indent=2)
print(f"  + downloads.json（{len(items)} 项）")
PY

ls -lh "$OUT"
