#!/usr/bin/env bash
# =============================================================================
#  deploy.sh : 把注册表服务端部署到 FTP（网页根目录）
#
#  需要环境变量（不写进仓库）：
#    LUX_FTP_HOST   主机（如 lux.xfes.top）
#    LUX_FTP_USER   FTP 用户名
#    LUX_FTP_PASS   FTP 密码
#    LUX_FTP_ROOT   站点内的目标目录，默认 /lux
#
#  用法：server/deploy.sh
#  会：生成下载包 -> 上传 PHP 源码 -> 上传 downloads/ -> 删除旧的 index.html
# =============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"

: "${LUX_FTP_HOST:?请设置 LUX_FTP_HOST}"
: "${LUX_FTP_USER:?请设置 LUX_FTP_USER}"
: "${LUX_FTP_PASS:?请设置 LUX_FTP_PASS}"
REMOTE_ROOT="${LUX_FTP_ROOT:-/lux}"
[[ "$REMOTE_ROOT" == /* ]] || REMOTE_ROOT="/$REMOTE_ROOT"
REMOTE_ROOT="${REMOTE_ROOT%/}"

FTP_BASE="ftp://$LUX_FTP_HOST"
AUTH=(-u "$LUX_FTP_USER:$LUX_FTP_PASS")

echo "== 生成下载包 =="
bash "$HERE/make_downloads.sh" >/dev/null
DL="$ROOT/dist/downloads"

up() {  # up <本地文件> <远端路径>
    curl -fsS --connect-timeout 20 -T "$1" "${AUTH[@]}" --ftp-create-dirs \
        "$FTP_BASE$REMOTE_ROOT/$2"
    echo "  ↑ $2"
}

echo "== 上传 PHP 源码 =="
for f in lux.php lib.php index.php README.md; do
    up "$HERE/$f" "$f"
done

echo "== 上传 downloads/ =="
for f in "$DL"/*; do
    [ -f "$f" ] || continue
    up "$f" "downloads/$(basename "$f")"
done

echo "== 删除旧的 index.html（改用 index.php） =="
curl -fsS --connect-timeout 20 "${AUTH[@]}" -Q "DELE $REMOTE_ROOT/index.html" \
    "$FTP_BASE/" 2>/dev/null && echo "  ✗ 已删除 index.html" || echo "  （index.html 不存在或删除失败，忽略）"

echo
echo "完成。自检："
curl -fsS "https://$LUX_FTP_HOST$REMOTE_ROOT/lux.php?action=health" || true
echo
