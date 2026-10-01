<?php
// =============================================================================
//  lib.php : Lux 包注册表 —— 公共库（账号 / 令牌 / 包存储）
//
//  index.php（网页界面）与 lux.php（luxc 调用的 API）共用本文件。
//  纯文件存储，不依赖数据库扩展；账号密码用 password_hash(bcrypt)。
//
//  目录结构：
//    data/users.json                 账号（以小写用户名为键）
//    data/tokens.json                令牌 SHA-256 -> 用户名（0.9.2 起不存明文）
//    data/ratelimit/<bucket>-<ip>.json  按 IP 的速率限制计数
//    packages/<名字>/<版本>.json     包元数据（含 owner）
//    packages/<名字>/<版本>.tar.gz   包归档
//    packages/<名字>/latest.json     最高版本元数据的副本
//    downloads/downloads.json        可下载的源码清单
// =============================================================================
declare(strict_types=1);

const LUX_SCHEMA  = 1;
const LUX_VERSION = '0.9.2';

function lux_pkg_dir(): string    { return __DIR__ . '/packages'; }
function lux_data_dir(): string   { return __DIR__ . '/data'; }
function lux_users_file(): string { return lux_data_dir() . '/users.json'; }
function lux_tokens_file(): string{ return lux_data_dir() . '/tokens.json'; }

function lux_ensure_dirs(): void {
    foreach ([lux_data_dir(), lux_pkg_dir()] as $d) {
        if (!is_dir($d)) @mkdir($d, 0755, true);
    }
}

// 包名 / 版本号只能是有界的路径分量
function valid_component(string $s): bool {
    return $s !== '' && strlen($s) <= 64 && $s !== '.' && $s !== '..'
        && preg_match('/^[A-Za-z0-9_.+-]+$/', $s) === 1;
}
function valid_version(string $s): bool {
    return valid_component($s) && preg_match('/^[0-9]/', $s) === 1;
}
function valid_username(string $s): bool {
    return preg_match('/^[A-Za-z0-9_]{3,32}$/', $s) === 1;
}

// -----------------------------------------------------------------------------
//  JSON 文件读写（写时加排他锁）
// -----------------------------------------------------------------------------
function json_read(string $file, array $default = []): array {
    if (!is_file($file)) return $default;
    $raw = @file_get_contents($file);
    if ($raw === false || $raw === '') return $default;
    $v = json_decode($raw, true);
    return is_array($v) ? $v : $default;
}

function json_write(string $file, $data): bool {
    $dir = dirname($file);
    if (!is_dir($dir)) @mkdir($dir, 0755, true);
    $fp = @fopen($file, 'c+');
    if (!$fp) return false;
    flock($fp, LOCK_EX);
    ftruncate($fp, 0);
    rewind($fp);
    fwrite($fp, json_encode($data, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT) . "\n");
    fflush($fp);
    flock($fp, LOCK_UN);
    fclose($fp);
    return true;
}

// -----------------------------------------------------------------------------
//  速率限制（0.9.2 加固）
//  最简的按 IP 文件计数：data/ratelimit/<bucket>-<hash(ip)>.json = {start,count}。
//  窗口内超过 max 次即拒绝。公网部署前够用；不做分布式共享计数。
// -----------------------------------------------------------------------------
function client_ip(): string {
    $ip = $_SERVER['REMOTE_ADDR'] ?? '0.0.0.0';
    return is_string($ip) && $ip !== '' ? $ip : '0.0.0.0';
}

function rate_limit(string $bucket, int $max, int $windowSec, string &$err): bool {
    $dir = lux_data_dir() . '/ratelimit';
    if (!is_dir($dir)) @mkdir($dir, 0755, true);
    $safe = preg_replace('/[^A-Za-z0-9_.-]/', '_', $bucket);
    $file = $dir . '/' . $safe . '-' . substr(hash('sha256', client_ip()), 0, 16) . '.json';
    $now = time();
    $st = json_read($file, ['start' => $now, 'count' => 0]);
    if (!isset($st['start']) || $now - (int)$st['start'] >= $windowSec) {
        $st = ['start' => $now, 'count' => 0];
    }
    $st['count'] = (int)($st['count'] ?? 0) + 1;
    json_write($file, $st);
    if ($st['count'] > $max) {
        $err = '操作过于频繁，请稍后再试';
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
//  账号
// -----------------------------------------------------------------------------
function users_load(): array { return json_read(lux_users_file(), []); }

function user_find(string $name): ?array {
    $u = users_load();
    $k = strtolower($name);
    return isset($u[$k]) ? $u[$k] : null;
}

function user_register(string $name, string $pass, string $email, string &$err): ?array {
    if (!rate_limit('register', 10, 3600, $err)) return null;
    if (!valid_username($name)) {
        $err = '用户名需为 3~32 位字母 / 数字 / 下划线';
        return null;
    }
    if (strlen($pass) < 6) {
        $err = '密码至少 6 位';
        return null;
    }
    if ($email !== '' && !filter_var($email, FILTER_VALIDATE_EMAIL)) {
        $err = '邮箱格式不正确';
        return null;
    }
    $users = users_load();
    $k = strtolower($name);
    if (isset($users[$k])) {
        $err = '用户名已被占用';
        return null;
    }
    $users[$k] = [
        'name'    => $name,
        'pass'    => password_hash($pass, PASSWORD_DEFAULT),
        'email'   => $email,
        'created' => gmdate('c'),
        'admin'   => count($users) === 0,  // 第一个注册者是管理员
    ];
    if (!json_write(lux_users_file(), $users)) {
        $err = '无法写入账号数据（data/ 是否可写？）';
        return null;
    }
    return $users[$k];
}

function user_verify(string $name, string $pass): ?array {
    $u = user_find($name);
    if (!$u || !isset($u['pass']) || !password_verify($pass, $u['pass'])) return null;
    return $u;
}

// -----------------------------------------------------------------------------
//  API 令牌
//  存储的是令牌的 SHA-256，而不是明文——即使 tokens.json 泄露，
//  也无法直接冒充发布者（0.9.2 加固）。
// -----------------------------------------------------------------------------
function tokens_load(): array { return json_read(lux_tokens_file(), []); }

function token_hash(string $token): string {
    return hash('sha256', $token);
}

function token_issue(string $user): string {
    $tokens = tokens_load();
    // 清理该用户旧令牌，避免无限增长
    foreach ($tokens as $t => $u) {
        if (strcasecmp($u, $user) === 0) unset($tokens[$t]);
    }
    $token = bin2hex(random_bytes(24));
    $tokens[token_hash($token)] = $user;  // 存散列，不存明文
    json_write(lux_tokens_file(), $tokens);
    return $token;
}

function token_revoke(string $token): void {
    $tokens = tokens_load();
    $k = token_hash($token);
    if (isset($tokens[$k])) {
        unset($tokens[$k]);
        json_write(lux_tokens_file(), $tokens);
    }
}

function token_user(?string $token): ?string {
    if (!$token) return null;
    $tokens = tokens_load();
    return $tokens[token_hash($token)] ?? null;
}

// 从请求里取令牌：Authorization: Bearer 或 POST/GET 的 token
function request_token(): ?string {
    $h = $_SERVER['HTTP_AUTHORIZATION'] ?? '';
    if (stripos($h, 'Bearer ') === 0) return trim(substr($h, 7));
    if (!empty($_POST['token'])) return (string)$_POST['token'];
    if (!empty($_GET['token']))  return (string)$_GET['token'];
    return null;
}

function request_user(): ?array {
    $name = token_user(request_token());
    return $name ? user_find($name) : null;
}

// -----------------------------------------------------------------------------
//  站点地址
// -----------------------------------------------------------------------------
function base_url(): string {
    $https = (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off')
        || (($_SERVER['HTTP_X_FORWARDED_PROTO'] ?? '') === 'https');
    $scheme = $https ? 'https' : 'http';
    $host = $_SERVER['HTTP_HOST'] ?? 'localhost';
    $dir = rtrim(str_replace('\\', '/', dirname($_SERVER['SCRIPT_NAME'] ?? '/')), '/');
    return $scheme . '://' . $host . $dir;
}

// -----------------------------------------------------------------------------
//  包存储
// -----------------------------------------------------------------------------
function pkg_meta_file(string $name, string $version): string {
    return lux_pkg_dir() . '/' . $name . '/' . $version . '.json';
}
function pkg_archive_file(string $name, string $version): string {
    return lux_pkg_dir() . '/' . $name . '/' . $version . '.tar.gz';
}

// 扫描全部版本；每个条目补 url / owner 等派生字段
function pkg_list_all(): array {
    $out = [];
    foreach (glob(lux_pkg_dir() . '/*', GLOB_ONLYDIR) ?: [] as $dir) {
        $name = basename($dir);
        if (!valid_component($name)) continue;
        foreach (glob($dir . '/*.json') ?: [] as $f) {
            if (basename($f) === 'latest.json') continue;
            $data = json_decode((string)@file_get_contents($f), true);
            if (!is_array($data)) continue;
            if (empty($data['name']))    $data['name'] = $name;
            if (empty($data['version'])) $data['version'] = basename($f, '.json');
            $data['owner'] = $data['owner'] ?? '';
            if (empty($data['url'])) {
                $data['url'] = base_url() . '/packages/' . rawurlencode($name) . '/'
                             . rawurlencode($data['version']) . '.tar.gz';
            }
            if (empty($data['sha256']) && is_file(pkg_archive_file($name, $data['version']))) {
                $data['sha256'] = hash_file('sha256', pkg_archive_file($name, $data['version']));
            }
            if (empty($data['size']) && is_file(pkg_archive_file($name, $data['version']))) {
                $data['size'] = filesize(pkg_archive_file($name, $data['version']));
            }
            $out[] = $data;
        }
    }
    usort($out, function ($a, $b) {
        $c = strcasecmp($a['name'], $b['name']);
        return $c !== 0 ? $c : version_compare($b['version'], $a['version']);
    });
    return $out;
}

// 每个包只保留最高版本
function pkg_latest_only(array $pkgs): array {
    $best = [];
    foreach ($pkgs as $p) {
        $n = $p['name'];
        if (!isset($best[$n]) || version_compare($p['version'], $best[$n]['version'], '>')) {
            $best[$n] = $p;
        }
    }
    return array_values($best);
}

function pkg_versions(string $name): array {
    $out = [];
    foreach (pkg_list_all() as $p) {
        if ($p['name'] === $name) $out[] = $p;
    }
    usort($out, fn($a, $b) => version_compare($b['version'], $a['version']));
    return $out;
}

function pkg_latest(string $name): ?array {
    $v = pkg_versions($name);
    return $v ? $v[0] : null;
}

function pkg_owned_by(string $name, string $version, string $user): bool {
    $f = pkg_meta_file($name, $version);
    if (!is_file($f)) return false;
    $m = json_decode((string)@file_get_contents($f), true);
    return is_array($m) && ($m['owner'] ?? '') === $user;
}

// 写入/更新元数据文件（不改动字段由调用方负责）
function pkg_write_meta(string $name, string $version, array $meta): bool {
    $dir = lux_pkg_dir() . '/' . $name;
    if (!is_dir($dir)) @mkdir($dir, 0755, true);
    $meta['name']    = $name;
    $meta['version'] = $version;
    $ok = json_write(pkg_meta_file($name, $version), $meta);
    // latest.json 只在当前版本是最高版本时刷新
    $latest = pkg_latest($name);
    if ($latest === null || version_compare($version, $latest['version'], '>=')) {
        json_write($dir . '/latest.json', $meta);
    }
    return $ok;
}

function pkg_remove_version(string $name, string $version): bool {
    @unlink(pkg_meta_file($name, $version));
    @unlink(pkg_archive_file($name, $version));
    $left = pkg_versions($name);
    if (!$left) {
        @unlink(lux_pkg_dir() . '/' . $name . '/latest.json');
        @rmdir(lux_pkg_dir() . '/' . $name);
        return true;
    }
    // 重建 latest.json
    $dir = lux_pkg_dir() . '/' . $name;
    @unlink($dir . '/latest.json');
    json_write($dir . '/latest.json', $left[0]);
    return true;
}

function pkg_remove_all(string $name): bool {
    $dir = lux_pkg_dir() . '/' . $name;
    if (!is_dir($dir)) return false;
    foreach (glob($dir . '/*') ?: [] as $f) @unlink($f);
    @rmdir($dir);
    return true;
}

function human_size($bytes): string {
    $bytes = (float)$bytes;
    foreach (['B', 'KB', 'MB', 'GB'] as $unit) {
        if ($bytes < 1024) {
            return ($unit === 'B' ? (string)(int)$bytes : number_format($bytes, 1)) . ' ' . $unit;
        }
        $bytes /= 1024;
    }
    return number_format($bytes, 1) . ' TB';
}

// -----------------------------------------------------------------------------
//  上传落盘（lux.php 的 publish 与 index.php 的上传表单共用）
//  $file 形如 $_FILES['archive']；$meta 为已解析的元数据数组。
//  $owner 为当前用户名；$admin=true 时能覆盖/修改别人的包。
// -----------------------------------------------------------------------------
function pkg_store_upload(array $file, array $meta, string $owner, bool $admin, string &$err): ?array {
    if (!rate_limit('upload', 60, 3600, $err)) return null;
    $name = (string)($meta['name'] ?? '');
    $version = (string)($meta['version'] ?? '');
    if (!valid_component($name)) { $err = '包名不合法（字母数字 _ - . +，不超过 64 字符）'; return null; }
    if (!valid_version($version)) { $err = '版本号不合法（需以数字开头）'; return null; }
    if (($file['error'] ?? 1) !== UPLOAD_ERR_OK) {
        $code = $file['error'] ?? 1;
        $err = in_array($code, [UPLOAD_ERR_INI_SIZE, UPLOAD_ERR_FORM_SIZE], true)
             ? '归档超过服务器允许的上传大小' : '缺少归档文件';
        return null;
    }
    $tmp = $file['tmp_name'] ?? '';
    if (!is_uploaded_file($tmp)) { $err = '非法的上传'; return null; }
    $size = filesize($tmp);
    if ($size <= 0 || $size > 16 * 1024 * 1024) { $err = '归档大小不合法（上限 16 MB）'; return null; }
    $fh = fopen($tmp, 'rb');
    $magic = fread($fh, 2);
    fclose($fh);
    if ($magic !== "\x1f\x8b") { $err = '归档必须是 gzip（.tar.gz / .tgz）'; return null; }

    $old = null;
    if (is_file(pkg_meta_file($name, $version))) {
        $old = json_decode((string)@file_get_contents(pkg_meta_file($name, $version)), true);
        if (!$admin && is_array($old) && !empty($old['owner']) && $old['owner'] !== $owner) {
            $err = "版本 $name@$version 属于 " . $old['owner'] . '，你不能覆盖';
            return null;
        }
    }

    $dest = pkg_archive_file($name, $version);
    if (!is_dir(dirname($dest))) @mkdir(dirname($dest), 0755, true);
    if (!move_uploaded_file($tmp, $dest)) { $err = '保存归档失败（packages/ 是否可写？）'; return null; }

    $meta['name']    = $name;
    $meta['version'] = $version;
    $meta['owner']   = $owner;
    $meta['url']     = base_url() . '/packages/' . rawurlencode($name) . '/' . rawurlencode($version) . '.tar.gz';
    $meta['sha256']  = hash_file('sha256', $dest);
    $meta['size']    = filesize($dest);
    $meta['updated_at'] = gmdate('c');
    $meta['created_at'] = (is_array($old) && !empty($old['created_at'])) ? $old['created_at'] : $meta['updated_at'];
    if (!pkg_write_meta($name, $version, $meta)) { $err = '写入元数据失败'; return null; }
    return $meta;
}

function h(?string $s): string {
    return htmlspecialchars((string)$s, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
}
