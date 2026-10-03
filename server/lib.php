<?php
// =============================================================================
//  lib.php : Lux 包注册表 —— 公共库（账号 / 令牌 / 包存储 / 打包 / 反馈）
//
//  index.php（网页界面）与 lux.php（luxc 调用的 API）共用本文件。
//  纯文件存储，不依赖数据库扩展；账号密码用 password_hash(bcrypt)。
//
//  目录结构：
//    data/users.php                  账号（以小写用户名为键）
//    data/tokens.php                 令牌 SHA-256 -> 用户名（0.9.2 起不存明文）
//    data/ratelimit/<bucket>-<ip>.php   按 IP 的速率限制计数
//    data/feedback.php               意见反馈（0.9.3）
//    以上数据文件均以 PHP 退出头（仅在 PHP 模式下直接退出）开头：即使站点没配置拒绝访问 data/，
//    直接请求也只会得到空响应（旧版 .json 会在首次请求时自动迁移后删除）。
//    packages/<名字>/<版本>.json     包元数据（含 owner）
//    packages/<名字>/<版本>.tar.gz   包归档
//    packages/<名字>/latest.json     最高版本元数据的副本
//    downloads/downloads.json        可下载的源码清单
//
//  0.9.3 起上传支持两种提供方式（网页与 API 一致）：
//    · 直接上传 .tar.gz 压缩包；
//    · 上传若干具体文件，由服务端纯 PHP 打成 .tar.gz（不依赖 Phar / tar 命令）；
//    两者都可选择「更新包」模式：只给改动文件，服务端在基础版本上叠加。
// =============================================================================
declare(strict_types=1);

const LUX_SCHEMA  = 1;
const LUX_VERSION = '1.2.0';

function lux_pkg_dir(): string    { return __DIR__ . '/packages'; }
function lux_data_dir(): string   { return __DIR__ . '/data'; }
// 0.9.3：数据文件改用 .php 后缀 + PHP 退出头守卫 —— 即使站点没有配置
// 拒绝访问 data/，直接请求也只会得到空响应，不会把账号 / 令牌 / 反馈读走。
function lux_users_file(): string { return lux_data_dir() . '/users.php'; }
function lux_tokens_file(): string{ return lux_data_dir() . '/tokens.php'; }
function lux_feedback_file(): string { return lux_data_dir() . '/feedback.php'; }

const LUX_JSON_GUARD = "<?php exit; ?>\n";

function lux_strip_guard(string $raw): string {
    if (strncmp($raw, '<?php', 5) === 0) {
        $eol = strpos($raw, "\n");
        return $eol === false ? '' : substr($raw, $eol + 1);
    }
    return $raw;
}

function lux_migrate_data_file(string $legacy, string $php): void {
    if (!is_file($legacy)) return;
    if (!is_file($php)) {
        $data = json_read($legacy, []);
        if ($data) json_write($php, $data);
    }
    @unlink($legacy);
}

function lux_ensure_dirs(): void {
    foreach ([lux_data_dir(), lux_pkg_dir(), lux_data_dir() . '/ratelimit'] as $d) {
        if (!is_dir($d)) @mkdir($d, 0755, true);
    }
    if (!is_file(lux_data_dir() . '/index.php')) {
        @file_put_contents(lux_data_dir() . '/index.php', "<?php http_response_code(404); exit;\n");
    }
    // 迁移旧版 .json 数据文件（改成带守卫的 .php，删除旧文件避免泄露）
    foreach ([lux_users_file(), lux_tokens_file(), lux_feedback_file()] as $php) {
        lux_migrate_data_file(substr($php, 0, -4) . '.json', $php);
    }
    foreach (glob(lux_data_dir() . '/ratelimit/*.json') ?: [] as $legacy) {
        lux_migrate_data_file($legacy, substr($legacy, 0, -5) . '.php');
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
//  .php 数据文件带 PHP 退出头守卫；读取时也会兼容同名旧 .json。
// -----------------------------------------------------------------------------
function json_read(string $file, array $default = []): array {
    $candidates = [$file];
    if (substr($file, -4) === '.php') $candidates[] = substr($file, 0, -4) . '.json';
    foreach ($candidates as $f) {
        if (!is_file($f)) continue;
        $raw = @file_get_contents($f);
        if ($raw === false || $raw === '') continue;
        $v = json_decode(lux_strip_guard($raw), true);
        if (is_array($v)) return $v;
    }
    return $default;
}

function json_write(string $file, $data): bool {
    $dir = dirname($file);
    if (!is_dir($dir)) @mkdir($dir, 0755, true);
    $fp = @fopen($file, 'c+');
    if (!$fp) return false;
    flock($fp, LOCK_EX);
    ftruncate($fp, 0);
    rewind($fp);
    $guard = (substr($file, -4) === '.php') ? LUX_JSON_GUARD : '';
    fwrite($fp, $guard . json_encode($data, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT) . "\n");
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
    $file = $dir . '/' . $safe . '-' . substr(hash('sha256', client_ip()), 0, 16) . '.php';
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
//  纯 PHP tar.gz 打包 / 解包（0.9.3）
//  用于「上传文件，站点打包」与「更新包叠加」，不依赖 Phar 扩展或 tar 命令。
//  只处理普通文件；目录隐含在路径里，符号链接等其余类型忽略。
// -----------------------------------------------------------------------------
function tar_safe_path(string $p): ?string {
    $p = str_replace('\\', '/', $p);
    while (strncmp($p, './', 2) === 0) $p = substr($p, 2);
    $p = preg_replace('#/+#', '/', $p) ?? $p;
    $p = trim($p);
    if ($p === '' || $p[0] === '/') return null;
    if (preg_match('#(^|/)\.\.(/|$)#', $p)) return null;
    if (preg_match('#^[A-Za-z]:#', $p)) return null;
    if (strlen($p) > 400) return null;
    return $p;
}

// ustar 头（512 字节）；路径放不下返回 ''，由调用方改用 GNU 长名
function tar_header(string $name, int $mode, int $size, int $mtime, string $type): string {
    $prefix = '';
    if (strlen($name) > 100) {
        $pos = strrpos(substr($name, 0, 156), '/');
        while ($pos !== false && strlen(substr($name, $pos + 1)) > 100) {
            $pos = strrpos(substr($name, 0, $pos), '/');
        }
        if ($pos === false) return '';
        $prefix = substr($name, 0, $pos);
        $name   = substr($name, $pos + 1);
    }
    $h  = str_pad($name, 100, "\0");
    $h .= sprintf("%07o\0", $mode & 0777777);
    $h .= sprintf("%07o\0", 0);
    $h .= sprintf("%07o\0", 0);
    $h .= sprintf("%011o\0", $size);
    $h .= sprintf("%011o\0", $mtime);
    $h .= '        ';
    $h .= $type;
    $h .= str_repeat("\0", 100);
    $h .= "ustar\0" . '00';
    $h .= str_repeat("\0", 32);
    $h .= str_repeat("\0", 32);
    $h .= sprintf("%07o\0", 0);
    $h .= sprintf("%07o\0", 0);
    $h .= str_pad($prefix, 155, "\0");
    $h .= str_repeat("\0", 12);
    $sum = 0;
    for ($i = 0; $i < 512; $i++) $sum += ord($h[$i]);
    return substr_replace($h, sprintf("%06o\0 ", $sum), 148, 8);
}

// $gz 为 gzopen 句柄
function tar_gz_put($gz, string $path, string $data, int $mode = 0644, int $mtime = 0): void {
    if ($mtime <= 0) $mtime = time();
    $blk = tar_header($path, $mode, strlen($data), $mtime, '0');
    if ($blk === '') {  // GNU 长名扩展头
        $long = $path . "\0";
        gzwrite($gz, tar_header('././@LongLink', 0644, strlen($long), $mtime, 'L'));
        gzwrite($gz, $long);
        $pad = (512 - (strlen($long) % 512)) % 512;
        if ($pad) gzwrite($gz, str_repeat("\0", $pad));
        $blk = tar_header(substr($path, -100), $mode, strlen($data), $mtime, '0');
        if ($blk === '') return;
    }
    gzwrite($gz, $blk);
    gzwrite($gz, $data);
    $pad = (512 - (strlen($data) % 512)) % 512;
    if ($pad) gzwrite($gz, str_repeat("\0", $pad));
}

// $entries: path => ['data'=>..., 'mode'=>..., 'mtime'=>...]
function tar_gz_write(string $dest, array $entries): bool {
    $gz = @gzopen($dest, 'wb6');
    if (!$gz) return false;
    foreach ($entries as $path => $e) {
        tar_gz_put($gz, (string)$path, (string)($e['data'] ?? ''),
                   (int)($e['mode'] ?? 0644), (int)($e['mtime'] ?? 0));
    }
    gzwrite($gz, str_repeat("\0", 1024));
    return gzclose($gz);
}

// 读 tar.gz -> path => ['data','mode','mtime']；不是有效归档时返回 null
function tar_gz_read(string $file): ?array {
    $gz = @gzopen($file, 'rb');
    if (!$gz) return null;
    $entries = [];
    $longName = null;
    $pax = [];
    $end = false;
    while (!$end && !gzeof($gz)) {
        $block = gzread($gz, 512);
        if ($block === false || strlen($block) < 512) break;
        if (rtrim($block, "\0") === '') break;
        $name   = rtrim(substr($block, 0, 100), "\0");
        $mode   = (int)octdec(trim(substr($block, 100, 8), "\0 "));
        $size   = (int)octdec(trim(substr($block, 124, 12), "\0 "));
        $mtime  = (int)octdec(trim(substr($block, 136, 12), "\0 "));
        $type   = substr($block, 156, 1);
        $prefix = rtrim(substr($block, 345, 155), "\0");
        if ($prefix !== '') $name = $prefix . '/' . $name;
        $data = '';
        if ($size > 0) {
            $left = $size;
            while ($left > 0) {
                $chunk = gzread($gz, min(65536, $left));
                if ($chunk === false || $chunk === '') { $end = true; break; }
                $data .= $chunk;
                $left -= strlen($chunk);
            }
            $pad = (512 - ($size % 512)) % 512;
            if ($pad > 0) gzread($gz, $pad);
        }
        if ($type === 'L') { $longName = rtrim($data, "\0"); continue; }
        if ($type === 'x' || $type === 'g') {
            foreach (preg_split('/\n/', $data) ?: [] as $line) {
                if (preg_match('/^\d+ (.*)$/', $line, $m)) $line = $m[1];
                $eq = strpos($line, '=');
                if ($eq !== false) $pax[substr($line, 0, $eq)] = substr($line, $eq + 1);
            }
            continue;
        }
        $path = $longName ?? ($pax['path'] ?? $name);
        $longName = null;
        $pax = [];
        // 只保留普通文件（'0' / NUL / '7'），目录与链接跳过
        if ($type !== '0' && $type !== "\0" && $type !== '' && $type !== '7') continue;
        $safe = tar_safe_path($path);
        if ($safe === null) continue;
        $entries[$safe] = ['data' => $data, 'mode' => $mode, 'mtime' => $mtime];
    }
    gzclose($gz);
    return $entries;
}

// 用表单元数据生成规范的包内 lux.json；保留上传/基础版本里的其它字段
function pkg_merge_lux_json(array $entries, array $meta): array {
    $existing = [];
    if (isset($entries['lux.json'])) {
        $j = json_decode((string)$entries['lux.json']['data'], true);
        if (is_array($j)) $existing = $j;
    }
    $out = $existing;
    foreach (['name', 'version', 'summary', 'description', 'main', 'license', 'homepage', 'lux'] as $k) {
        if (isset($meta[$k]) && $meta[$k] !== '') $out[$k] = $meta[$k];
    }
    foreach (['files', 'tags', 'authors'] as $k) {
        if (!empty($meta[$k]) && is_array($meta[$k])) $out[$k] = array_values($meta[$k]);
    }
    if (!empty($meta['deps']) && is_array($meta['deps'])) $out['deps'] = $meta['deps'];
    $out['name']    = (string)($meta['name'] ?? ($out['name'] ?? ''));
    $out['version'] = (string)($meta['version'] ?? ($out['version'] ?? ''));
    $entries['lux.json'] = [
        'data'  => json_encode($out, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT) . "\n",
        'mode'  => 0644,
        'mtime' => time(),
    ];
    return $entries;
}

function pkg_order_entries(array $entries): array {
    $out = [];
    if (isset($entries['lux.json'])) $out['lux.json'] = $entries['lux.json'];
    ksort($entries, SORT_STRING);
    foreach ($entries as $k => $v) if ($k !== 'lux.json') $out[$k] = $v;
    return $out;
}

// $_FILES['files']（数组形式）-> 一维文件列表
// PHP 8.1+ 对整目录上传会把相对路径放在 full_path，优先用它
function normalize_files_array(?array $f): array {
    if (!$f || !isset($f['name'])) return [];
    if (!is_array($f['name'])) {
        $full = (string)($f['full_path'] ?? '');
        return [['name' => $full !== '' ? $full : (string)$f['name'],
                 'tmp_name' => (string)($f['tmp_name'] ?? ''),
                 'error' => (int)($f['error'] ?? 1), 'size' => (int)($f['size'] ?? 0)]];
    }
    $out = [];
    foreach ($f['name'] as $i => $n) {
        $full = (string)($f['full_path'][$i] ?? '');
        $out[] = ['name' => $full !== '' ? $full : (string)$n,
                  'tmp_name' => (string)($f['tmp_name'][$i] ?? ''),
                  'error' => (int)($f['error'][$i] ?? 1),
                  'size' => (int)($f['size'][$i] ?? 0)];
    }
    return $out;
}

// 读取上传的普通文件，返回 [['name'=>原始名, 'data'=>内容], ...]
function pkg_read_uploads(?array $raw, string &$err): ?array {
    $out = [];
    $total = 0;
    foreach (normalize_files_array($raw) as $f) {
        if (($f['error'] ?? 1) === UPLOAD_ERR_NO_FILE) continue;
        if (($f['error'] ?? 1) !== UPLOAD_ERR_OK) {
            $err = '文件上传失败：' . $f['name'];
            return null;
        }
        if (!is_uploaded_file($f['tmp_name'])) { $err = '非法的上传：' . $f['name']; return null; }
        $size = @filesize($f['tmp_name']);
        if ($size === false || $size > 8 * 1024 * 1024) {
            $err = '单个文件超过 8 MB：' . $f['name'];
            return null;
        }
        $total += (int)$size;
        if ($total > 16 * 1024 * 1024) { $err = '文件总大小超过 16 MB'; return null; }
        $data = @file_get_contents($f['tmp_name']);
        if ($data === false) { $err = '读取上传文件失败：' . $f['name']; return null; }
        $out[] = ['name' => (string)$f['name'], 'data' => $data];
    }
    // 整个文件夹上传时名称形如 "pkg/lib.lux"，若所有路径同属一个顶层目录就去掉它
    if ($out) {
        $prefix = null;
        $shared = true;
        foreach ($out as $f) {
            $p = tar_safe_path((string)$f['name']);
            if ($p === null) { $shared = false; break; }
            $slash = strpos($p, '/');
            if ($slash === false) { $shared = false; break; }
            $top = substr($p, 0, $slash);
            if ($prefix === null) $prefix = $top;
            elseif ($prefix !== $top) { $shared = false; break; }
        }
        if ($shared && $prefix !== null) {
            foreach ($out as &$f) {
                $p = tar_safe_path((string)$f['name']);
                $f['name'] = substr($p, strlen($prefix) + 1);
            }
            unset($f);
        }
    }
    return $out;
}

// 基础版本归档 + 新文件 / 更新归档 叠加，返回完整 entries
function pkg_entries_with_base(string $name, string $baseVersion, ?string $archiveTmp,
                               array $uploadedFiles, string &$err): ?array {
    $entries = [];
    if ($baseVersion !== '') {
        if (!valid_version($baseVersion)) { $err = '基础版本号不合法'; return null; }
        $baseFile = pkg_archive_file($name, $baseVersion);
        if (!is_file($baseFile) || !is_file(pkg_meta_file($name, $baseVersion))) {
            $err = "基础版本 $name@$baseVersion 不存在";
            return null;
        }
        $base = tar_gz_read($baseFile);
        if ($base === null) { $err = '基础版本归档损坏，无法作为更新基准'; return null; }
        $entries = $base;
    }
    if ($archiveTmp !== null && $archiveTmp !== '') {
        $add = tar_gz_read($archiveTmp);
        if ($add === null) { $err = '更新包无法解析（需要 .tar.gz / .tgz）'; return null; }
        foreach ($add as $p => $e) $entries[$p] = $e;
    }
    foreach ($uploadedFiles as $f) {
        $p = tar_safe_path((string)$f['name']);
        if ($p === null) { $err = '文件名不合法：' . (string)$f['name']; return null; }
        $entries[$p] = ['data' => (string)$f['data'], 'mode' => 0644, 'mtime' => time()];
    }
    return $entries;
}

// -----------------------------------------------------------------------------
//  上传落盘（lux.php 的 publish 与 index.php 的上传表单共用）
//  三种发布形态：
//    · 完整包 + 压缩包   pkg_store_upload($file, ...)
//    · 完整包 + 文件列表 pkg_store_files($files, ...)
//    · 更新包 + （压缩包或文件）+ 基础版本 $baseVersion
//  $owner 为当前用户名；$admin=true 时能覆盖/修改别人的包。
// -----------------------------------------------------------------------------
function pkg_finalize(string $tmpFile, array $meta, string $owner, bool $admin, string &$err): ?array {
    $name = (string)($meta['name'] ?? '');
    $version = (string)($meta['version'] ?? '');
    if (!valid_component($name)) { $err = '包名不合法（字母数字 _ - . +，不超过 64 字符）'; return null; }
    if (!valid_version($version)) { $err = '版本号不合法（需以数字开头）'; return null; }
    $size = @filesize($tmpFile);
    if ($size === false || $size <= 0 || $size > 16 * 1024 * 1024) {
        $err = '归档大小不合法（上限 16 MB）';
        return null;
    }
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
    if (!@copy($tmpFile, $dest)) { $err = '保存归档失败（packages/ 是否可写？）'; return null; }

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

// 生成一个临时文件路径（调用方负责删除）
function pkg_temp_file(): ?string {
    $t = @tempnam(sys_get_temp_dir(), 'luxpkg');
    return $t === false ? null : $t;
}

// 直接上传 .tar.gz。$baseVersion 非空时按「更新包」处理：在基础版本上叠加
function pkg_store_upload(array $file, array $meta, string $owner, bool $admin,
                          string &$err, ?string $baseVersion = null): ?array {
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
    $tmp = (string)($file['tmp_name'] ?? '');
    if (!is_uploaded_file($tmp)) { $err = '非法的上传'; return null; }
    $size = @filesize($tmp);
    if ($size === false || $size <= 0 || $size > 16 * 1024 * 1024) {
        $err = '归档大小不合法（上限 16 MB）';
        return null;
    }
    $fh = @fopen($tmp, 'rb');
    $magic = $fh ? fread($fh, 2) : '';
    if ($fh) fclose($fh);
    if ($magic !== "\x1f\x8b") { $err = '归档必须是 gzip（.tar.gz / .tgz）'; return null; }

    $work = $tmp;
    $generated = null;
    $entries = pkg_entries_with_base($name, (string)$baseVersion, $tmp, [], $err);
    if ($entries === null) return null;
    $generated = pkg_temp_file();
    if ($generated === null) { $err = '无法创建临时文件'; return null; }
    if (!tar_gz_write($generated, pkg_order_entries(pkg_merge_lux_json($entries, $meta)))) {
        @unlink($generated);
        $err = '重新打包失败';
        return null;
    }
    $work = $generated;
    $saved = pkg_finalize($work, $meta, $owner, $admin, $err);
    @unlink($generated);
    return $saved;
}

// 上传若干具体文件，服务端打成 .tar.gz；$baseVersion 非空时在其上叠加
function pkg_store_files(array $fileSet, array $meta, string $owner, bool $admin,
                         ?string $baseVersion, string &$err): ?array {
    if (!rate_limit('upload', 60, 3600, $err)) return null;
    $name = (string)($meta['name'] ?? '');
    $version = (string)($meta['version'] ?? '');
    if (!valid_component($name)) { $err = '包名不合法（字母数字 _ - . +，不超过 64 字符）'; return null; }
    if (!valid_version($version)) { $err = '版本号不合法（需以数字开头）'; return null; }
    $uploads = pkg_read_uploads($fileSet, $err);
    if ($uploads === null) return null;
    if (count($uploads) > 300) { $err = '文件数过多（上限 300 个）'; return null; }
    $base = (string)$baseVersion;
    if (!$uploads && $base === '') { $err = '没有选择任何文件'; return null; }

    $entries = pkg_entries_with_base($name, $base, null, $uploads, $err);
    if ($entries === null) return null;
    if (!$entries) { $err = '没有可打包的内容'; return null; }
    // 元数据没写源文件时，用上传的文件名补全
    if (empty($meta['files'])) {
        $fs = [];
        foreach ($entries as $p => $e) if ($p !== 'lux.json') $fs[] = $p;
        sort($fs, SORT_STRING);
        $meta['files'] = $fs;
    }
    $generated = pkg_temp_file();
    if ($generated === null) { $err = '无法创建临时文件'; return null; }
    if (!tar_gz_write($generated, pkg_order_entries(pkg_merge_lux_json($entries, $meta)))) {
        @unlink($generated);
        $err = '打包失败';
        return null;
    }
    $saved = pkg_finalize($generated, $meta, $owner, $admin, $err);
    @unlink($generated);
    return $saved;
}

// -----------------------------------------------------------------------------
//  意见反馈（0.9.3）
//  任何人可提交（按 IP 限流），管理员可标记处理 / 删除。
// -----------------------------------------------------------------------------
function feedback_types(): array { return ['建议', '问题', '其他']; }

function feedback_add(string $type, string $content, string $contact, string $user, string &$err): ?array {
    if (!rate_limit('feedback', 5, 3600, $err)) return null;
    $content = trim($content);
    if ($content === '') { $err = '反馈内容不能为空'; return null; }
    if (strlen($content) > 8000) { $err = '反馈内容太长（上限约 8000 字节）'; return null; }
    if (!in_array($type, feedback_types(), true)) $type = '其他';
    $contact = trim($contact);
    if (strlen($contact) > 200) $contact = substr($contact, 0, 200);
    $item = [
        'id'      => bin2hex(random_bytes(6)),
        'type'    => $type,
        'content' => $content,
        'contact' => $contact,
        'user'    => $user,
        'time'    => gmdate('c'),
        'status'  => 'open',
    ];
    $all = json_read(lux_feedback_file(), []);
    $all[] = $item;
    if (!json_write(lux_feedback_file(), $all)) { $err = '反馈保存失败（data/ 是否可写？）'; return null; }
    return $item;
}

function feedback_list(): array {
    $all = json_read(lux_feedback_file(), []);
    usort($all, fn($a, $b) => strcmp((string)($b['time'] ?? ''), (string)($a['time'] ?? '')));
    return $all;
}

function feedback_set_status(string $id, string $status): bool {
    $all = json_read(lux_feedback_file(), []);
    $hit = false;
    foreach ($all as &$f) {
        if (($f['id'] ?? '') === $id) { $f['status'] = $status; $hit = true; }
    }
    unset($f);
    return $hit && json_write(lux_feedback_file(), $all);
}

function feedback_delete(string $id): bool {
    $all = json_read(lux_feedback_file(), []);
    $out = array_values(array_filter($all, fn($f) => ($f['id'] ?? '') !== $id));
    if (count($out) === count($all)) return false;
    return json_write(lux_feedback_file(), $out);
}

function h(?string $s): string {
    return htmlspecialchars((string)$s, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
}
