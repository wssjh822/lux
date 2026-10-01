<?php
// =============================================================================
//  index.php : Lux 包注册表 —— 网页界面（0.9.2）
//
//  在线登录 / 注册 / 浏览 / 搜索 / 上传 / 修改 / 删除自己的包；
//  也可查看与下载别人的包。上传需要账号，且只能改删自己发布的包。
//
//  另提供：包管理系统源码下载、Lux 各版本源码下载。
// =============================================================================
declare(strict_types=1);
require __DIR__ . '/lib.php';
lux_ensure_dirs();
session_start();

// -----------------------------------------------------------------------------
//  小工具
// -----------------------------------------------------------------------------
function csrf(): string {
    if (empty($_SESSION['csrf'])) $_SESSION['csrf'] = bin2hex(random_bytes(16));
    return $_SESSION['csrf'];
}
function check_csrf(): void {
    if (!hash_equals((string)($_SESSION['csrf'] ?? ''), (string)($_POST['csrf'] ?? ''))) {
        http_response_code(400);
        exit('CSRF 校验失败，请刷新页面重试。');
    }
}
function flash(?string $msg = null, string $type = 'ok') {
    if ($msg === null) return $_SESSION['flash'] ?? null;
    $_SESSION['flash'] = ['msg' => $msg, 'type' => $type];
    return null;
}
function take_flash(): ?array {
    $f = $_SESSION['flash'] ?? null;
    unset($_SESSION['flash']);
    return is_array($f) ? $f : null;
}
function redirect(string $url): void { header('Location: ' . $url); exit; }
function current_user(): ?array {
    if (empty($_SESSION['user'])) return null;
    return user_find((string)$_SESSION['user']);
}
function url_with(array $q): string {
    return '?' . http_build_query($q);
}
// 逗号 / 换行分隔 -> 数组
function lines_to_array(?string $s): array {
    $parts = preg_split('/[\r\n,]+/', (string)$s) ?: [];
    return array_values(array_filter(array_map('trim', $parts), fn($x) => $x !== ''));
}
function deps_to_text(array $deps): string {
    $out = [];
    foreach ($deps as $k => $v) $out[] = $k . (($v === '' || $v === '*') ? '' : '@' . $v);
    return implode("\n", $out);
}
function deps_from_text(?string $s): array {
    $deps = [];
    foreach (preg_split('/[\r\n]+/', (string)$s) ?: [] as $line) {
        $line = trim($line);
        if ($line === '') continue;
        $at = strrpos($line, '@');
        if ($at !== false && $at > 0) $deps[substr($line, 0, $at)] = trim(substr($line, $at + 1));
        else $deps[$line] = '*';
    }
    return $deps;
}

$user = current_user();

// -----------------------------------------------------------------------------
//  POST 处理
// -----------------------------------------------------------------------------
if (($_SERVER['REQUEST_METHOD'] ?? 'GET') === 'POST') {
    $do = (string)($_POST['do'] ?? '');
    check_csrf();

    if ($do === 'register' || $do === 'login') {
        $name = trim((string)($_POST['username'] ?? ''));
        $pass = (string)($_POST['password'] ?? '');
        if ($do === 'register') {
            $err = '';
            $u = user_register($name, $pass, trim((string)($_POST['email'] ?? '')), $err);
            if (!$u) { flash($err, 'err'); redirect('?p=register'); }
            $_SESSION['user'] = $u['name'];
            flash('注册成功，欢迎 ' . $u['name'] . '！');
            redirect('?p=dashboard');
        }
        $u = user_verify($name, $pass);
        if (!$u) { flash('用户名或密码错误', 'err'); redirect('?p=login'); }
        $_SESSION['user'] = $u['name'];
        flash('已登录：' . $u['name']);
        redirect('?p=dashboard');
    }

    if ($do === 'logout') {
        unset($_SESSION['user']);
        flash('已退出登录');
        redirect('?p=home');
    }

    if (!$user) { flash('请先登录', 'err'); redirect('?p=login'); }

    if ($do === 'upload') {
        $meta = [
            'name'        => trim((string)($_POST['name'] ?? '')),
            'version'     => trim((string)($_POST['version'] ?? '')),
            'summary'     => trim((string)($_POST['summary'] ?? '')),
            'description' => (string)($_POST['description'] ?? ''),
            'main'        => trim((string)($_POST['main'] ?? 'lib.lux')),
            'license'     => trim((string)($_POST['license'] ?? '')),
            'homepage'    => trim((string)($_POST['homepage'] ?? '')),
            'lux'         => trim((string)($_POST['lux'] ?? '')),
            'files'       => lines_to_array($_POST['files'] ?? ''),
            'tags'        => lines_to_array($_POST['tags'] ?? ''),
            'authors'     => lines_to_array($_POST['authors'] ?? ''),
            'deps'        => deps_from_text($_POST['deps'] ?? ''),
        ];
        $err = '';
        $saved = pkg_store_upload($_FILES['archive'] ?? [], $meta, $user['name'],
                                  !empty($user['admin']), $err);
        if (!$saved) { flash($err, 'err'); redirect('?p=upload'); }
        flash('已发布 ' . $saved['name'] . '@' . $saved['version']);
        redirect('?p=pkg&name=' . urlencode($saved['name']));
    }

    if ($do === 'edit') {
        $name = (string)($_POST['name'] ?? '');
        $version = (string)($_POST['version'] ?? '');
        if (!valid_component($name) || !valid_version($version) || !is_file(pkg_meta_file($name, $version))) {
            flash('包或版本不存在', 'err'); redirect('?p=dashboard');
        }
        if (!pkg_owned_by($name, $version, $user['name']) && empty($user['admin'])) {
            flash('只能修改自己发布的包', 'err'); redirect('?p=dashboard');
        }
        $meta = json_decode((string)@file_get_contents(pkg_meta_file($name, $version)), true) ?: [];
        foreach (['summary', 'description', 'main', 'license', 'homepage', 'lux'] as $f) {
            if (array_key_exists($f, $_POST)) $meta[$f] = trim((string)$_POST[$f]);
        }
        foreach (['files', 'tags', 'authors'] as $f) {
            if (array_key_exists($f, $_POST)) $meta[$f] = lines_to_array($_POST[$f]);
        }
        if (array_key_exists('deps', $_POST)) $meta['deps'] = deps_from_text($_POST['deps']);
        $meta['updated_at'] = gmdate('c');
        if (!pkg_write_meta($name, $version, $meta)) { flash('写入失败', 'err'); redirect('?p=dashboard'); }
        flash('已更新 ' . $name . '@' . $version);
        redirect('?p=pkg&name=' . urlencode($name));
    }

    if ($do === 'delete') {
        $name = (string)($_POST['name'] ?? '');
        $version = trim((string)($_POST['version'] ?? ''));
        if (!valid_component($name)) { flash('非法包名', 'err'); redirect('?p=dashboard'); }
        $vers = pkg_versions($name);
        if (!$vers) { flash('包不存在', 'err'); redirect('?p=dashboard'); }
        if ($version !== '') {
            if (!pkg_owned_by($name, $version, $user['name']) && empty($user['admin'])) {
                flash('只能删除自己发布的包', 'err'); redirect('?p=dashboard');
            }
            pkg_remove_version($name, $version);
            flash('已删除 ' . $name . '@' . $version);
        } else {
            foreach ($vers as $v) {
                if (($v['owner'] ?? '') !== $user['name'] && empty($user['admin'])) {
                    flash('包里有不是你的版本，不能整包删除', 'err'); redirect('?p=dashboard');
                }
            }
            pkg_remove_all($name);
            flash('已删除包 ' . $name);
        }
        redirect('?p=dashboard');
    }
}

// -----------------------------------------------------------------------------
//  页面数据
// -----------------------------------------------------------------------------
$page = (string)($_GET['p'] ?? 'home');
$flash = take_flash();
$all = pkg_list_all();
$latest = pkg_latest_only($all);
$sitePkgs = count($latest);
$siteVers = count($all);

$downloads = json_read(__DIR__ . '/downloads/downloads.json', []);
$downloads = $downloads['downloads'] ?? $downloads;
if (!is_array($downloads)) $downloads = [];

function page_title(string $p): string {
    return [
        'home' => '首页', 'browse' => '浏览包', 'pkg' => '包详情',
        'downloads' => '下载', 'login' => '登录', 'register' => '注册',
        'dashboard' => '我的包', 'upload' => '上传包', 'edit' => '编辑包',
        'about' => '关于',
    ][$p] ?? '首页';
}

// 渲染头
function render_head(string $page, ?array $user, ?array $flash): void { ?>
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title><?= h(page_title($page)) ?> · Lux 包注册表</title>
<style>
:root{
  --bg:#0b0e17; --panel:#151a29; --panel2:#1c2333; --line:#2a3348;
  --fg:#e8ecf5; --muted:#96a0b8; --accent:#7c5cff; --accent2:#3aa0ff;
  --ok:#37d67a; --err:#ff5c7a; --gold:#ffce5c;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;
  line-height:1.6;font-size:15px}
a{color:var(--accent2);text-decoration:none}
a:hover{text-decoration:underline}
code,pre{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
header.top{position:sticky;top:0;z-index:20;backdrop-filter:blur(10px);
  background:rgba(11,14,23,.85);border-bottom:1px solid var(--line)}
.wrap{max-width:1080px;margin:0 auto;padding:0 20px}
.nav{display:flex;align-items:center;gap:8px;height:58px}
.brand{display:flex;align-items:center;gap:10px;font-weight:700;font-size:18px;margin-right:14px}
.brand .dot{width:26px;height:26px;border-radius:8px;
  background:linear-gradient(135deg,var(--accent),var(--accent2));display:grid;place-items:center;color:#fff;font-size:14px}
.nav a.link{padding:7px 12px;border-radius:9px;color:var(--muted)}
.nav a.link:hover{background:var(--panel);color:var(--fg);text-decoration:none}
.nav .spacer{flex:1}
.btn{display:inline-block;padding:8px 15px;border-radius:9px;border:1px solid var(--line);
  background:var(--panel);color:var(--fg);cursor:pointer;font-size:14px;line-height:1.2}
.btn:hover{background:var(--panel2);text-decoration:none}
.btn.primary{background:linear-gradient(135deg,var(--accent),var(--accent2));border:0;color:#fff;font-weight:600}
.btn.danger{border-color:#5a2436;color:#ff9db2}
.btn.small{padding:4px 10px;font-size:13px}
.hero{padding:56px 0 34px;text-align:center}
.hero h1{font-size:38px;margin:0 0 10px;letter-spacing:.5px}
.hero p{color:var(--muted);margin:0 auto;max-width:640px}
.searchbar{display:flex;gap:10px;max-width:600px;margin:26px auto 0}
.searchbar input{flex:1;padding:12px 16px;border-radius:11px;border:1px solid var(--line);
  background:var(--panel);color:var(--fg);font-size:15px;outline:none}
.searchbar input:focus{border-color:var(--accent)}
.stats{display:flex;gap:26px;justify-content:center;margin-top:22px;color:var(--muted);font-size:14px}
.stats b{color:var(--fg)}
main{padding:30px 0 70px}
h2.section{font-size:20px;margin:34px 0 16px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(305px,1fr));gap:16px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:18px;
  transition:border-color .15s,transform .15s}
.card:hover{border-color:#3b4770;transform:translateY(-2px)}
.card h3{margin:0 0 4px;font-size:17px}
.card h3 a{color:var(--fg)}
.card .sum{color:var(--muted);font-size:13.5px;min-height:40px}
.meta{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:12px;font-size:12.5px;color:var(--muted)}
.pill{display:inline-block;background:var(--panel2);border:1px solid var(--line);
  border-radius:999px;padding:2px 10px;font-size:12px;color:var(--muted)}
.pill.v{color:var(--gold);border-color:#4a4020}
.pill.owner{color:var(--ok);border-color:#214a33}
.tag{color:var(--accent2)}
table{width:100%;border-collapse:collapse;margin:8px 0;font-size:14px}
th,td{text-align:left;padding:9px 12px;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--muted);font-weight:600;font-size:12.5px;text-transform:uppercase;letter-spacing:.4px}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:22px;margin:18px 0}
.form{display:grid;gap:14px;max-width:720px}
.form label{display:block;font-size:13px;color:var(--muted);margin-bottom:5px}
.form input,.form textarea,.form select{width:100%;padding:10px 12px;border-radius:9px;
  border:1px solid var(--line);background:var(--panel2);color:var(--fg);font-size:14px;outline:none}
.form input:focus,.form textarea:focus{border-color:var(--accent)}
.form textarea{min-height:84px;resize:vertical;font-family:inherit}
.form .row{display:grid;grid-template-columns:1fr 1fr;gap:14px}
.hint{color:var(--muted);font-size:12.5px;margin-top:4px}
.flash{padding:11px 15px;border-radius:10px;margin:16px 0;font-size:14px}
.flash.ok{background:#12301f;border:1px solid #1f5a37;color:#9ff0bf}
.flash.err{background:#33121c;border:1px solid #5a2436;color:#ffb3c4}
.breadcrumb{color:var(--muted);font-size:13px;margin-bottom:10px}
.breadcrumb a{color:var(--muted)}
.kv{display:grid;grid-template-columns:130px 1fr;gap:6px 14px;font-size:14px}
.kv .k{color:var(--muted)}
pre.code{background:#0d111c;border:1px solid var(--line);border-radius:11px;padding:14px 16px;overflow:auto;font-size:13px}
.empty{color:var(--muted);text-align:center;padding:40px 0}
footer{border-top:1px solid var(--line);color:var(--muted);font-size:13px;padding:26px 0;text-align:center}
.dl{display:flex;justify-content:space-between;align-items:center;gap:14px;padding:14px 0;border-bottom:1px solid var(--line)}
.dl:last-child{border-bottom:0}
.dl .t{font-weight:600}
.dl .d{color:var(--muted);font-size:13px}
@media(max-width:640px){.form .row{grid-template-columns:1fr}.hero h1{font-size:30px}}
</style>
</head>
<body>
<header class="top"><div class="wrap nav">
  <a class="brand" href="?p=home"><span class="dot">λ</span> Lux 包注册表</a>
  <a class="link" href="?p=browse">浏览</a>
  <a class="link" href="?p=downloads">下载</a>
  <a class="link" href="?p=about">关于</a>
  <span class="spacer"></span>
  <?php if ($user): ?>
    <a class="link" href="?p=dashboard"><?= h($user['name']) ?></a>
    <a class="btn small" href="?p=upload">上传包</a>
    <form method="post" style="display:inline">
      <input type="hidden" name="csrf" value="<?= h(csrf()) ?>">
      <input type="hidden" name="do" value="logout">
      <button class="btn small" type="submit">退出</button>
    </form>
  <?php else: ?>
    <a class="link" href="?p=login">登录</a>
    <a class="btn primary small" href="?p=register">注册</a>
  <?php endif; ?>
</div></header>
<main class="wrap">
<?php if ($flash): ?>
  <div class="flash <?= h($flash['type']) ?>"><?= h($flash['msg']) ?></div>
<?php endif; ?>
<?php }

function render_foot(): void { ?>
</main>
<footer>
  <div class="wrap">
    Lux 包注册表 v<?= h(LUX_VERSION) ?> · 上传需注册账号，只能修改 / 删除自己的包 ·
    API：<code>lux.php</code> · <a href="?p=downloads">源码下载</a>
  </div>
</footer>
</body></html>
<?php }

// -----------------------------------------------------------------------------
//  各种页面片段
// -----------------------------------------------------------------------------
function render_pkg_card(array $p): void { ?>
  <div class="card">
    <h3><a href="?p=pkg&amp;name=<?= urlencode($p['name']) ?>"><?= h($p['name']) ?></a></h3>
    <div class="sum"><?= h($p['summary'] ?? '') ?></div>
    <div class="meta">
      <span class="pill v">v<?= h($p['version']) ?></span>
      <?php if (!empty($p['owner'])): ?><span class="pill owner">@<?= h($p['owner']) ?></span><?php endif; ?>
      <?php foreach (array_slice($p['tags'] ?? [], 0, 3) as $t): ?>
        <span class="tag">#<?= h($t) ?></span>
      <?php endforeach; ?>
    </div>
  </div>
<?php }

// -----------------------------------------------------------------------------
//  路由
// -----------------------------------------------------------------------------
render_head($page, $user, $flash);

switch ($page) {

case 'home': ?>
  <section class="hero">
    <h1>Lux 包注册表</h1>
    <p>为 <code>luxc</code> 提供包索引与归档：在线注册、上传、修改、删除自己的包，
       浏览与下载别人的包。也提供编译器与包管理系统源码下载。</p>
    <form class="searchbar" method="get" action="">
      <input type="hidden" name="p" value="browse">
      <input type="text" name="q" placeholder="搜索包名 / 简介 / 标签…">
      <button class="btn primary" type="submit">搜索</button>
    </form>
    <div class="stats"><span><b><?= $sitePkgs ?></b> 个包</span><span><b><?= $siteVers ?></b> 个版本</span></div>
  </section>
  <h2 class="section">全部包</h2>
  <?php if (!$latest): ?>
    <div class="empty">还没有包。登录后点右上角「上传包」发布第一个。</div>
  <?php else: ?>
    <div class="grid"><?php foreach ($latest as $p) render_pkg_card($p); ?></div>
  <?php endif;
  break;

case 'browse':
  $q = trim((string)($_GET['q'] ?? ''));
  $list = $latest;
  if ($q !== '') {
    $lq = function_exists('mb_strtolower') ? mb_strtolower($q, 'UTF-8') : strtolower($q);
    $list = array_values(array_filter($list, function ($p) use ($lq) {
      $hay = ($p['name'] ?? '') . ' ' . ($p['summary'] ?? '') . ' ' . ($p['description'] ?? '') . ' '
           . implode(' ', $p['tags'] ?? []);
      $hay = function_exists('mb_strtolower') ? mb_strtolower($hay, 'UTF-8') : strtolower($hay);
      return function_exists('mb_strpos') ? mb_strpos($hay, $lq) !== false : strpos($hay, $lq) !== false;
    }));
  } ?>
  <h2 class="section"><?= $q === '' ? '全部包' : '搜索：' . h($q) ?></h2>
  <?php if (!$list): ?><div class="empty">没有找到匹配的包。</div><?php else: ?>
    <div class="grid"><?php foreach ($list as $p) render_pkg_card($p); ?></div>
  <?php endif;
  break;

case 'pkg':
  $name = (string)($_GET['name'] ?? '');
  $vers = valid_component($name) ? pkg_versions($name) : [];
  if (!$vers) { echo '<div class="empty">包不存在。<a href="?p=browse">返回浏览</a></div>'; break; }
  $latestP = $vers[0]; ?>
  <div class="breadcrumb"><a href="?p=browse">浏览</a> / <?= h($name) ?></div>
  <div class="panel">
    <h1 style="margin:0 0 6px;font-size:26px"><?= h($name) ?>
      <span class="pill v" style="font-size:13px;vertical-align:middle">v<?= h($latestP['version']) ?></span>
    </h1>
    <p style="color:var(--muted);margin:0 0 16px"><?= h($latestP['summary'] ?? '') ?></p>
    <div class="meta" style="margin-bottom:16px">
      <?php if (!empty($latestP['owner'])): ?><span class="pill owner">@<?= h($latestP['owner']) ?></span><?php endif; ?>
      <?php if (!empty($latestP['license'])): ?><span class="pill"><?= h($latestP['license']) ?></span><?php endif; ?>
      <?php if (!empty($latestP['lux'])): ?><span class="pill">Lux <?= h($latestP['lux']) ?></span><?php endif; ?>
      <?php foreach ($latestP['tags'] ?? [] as $t): ?><span class="tag">#<?= h($t) ?></span><?php endforeach; ?>
    </div>
    <?php if (!empty($latestP['description'])): ?>
      <pre class="code" style="white-space:pre-wrap"><?= h($latestP['description']) ?></pre>
    <?php endif; ?>
    <h3 style="margin:20px 0 4px;font-size:16px">安装</h3>
    <pre class="code">luxc install <?= h($name) ?><?= $latestP['version'] ? '@' . h($latestP['version']) : '' ?></pre>

    <h3 style="margin:22px 0 4px;font-size:16px">版本（<?= count($vers) ?>）</h3>
    <table>
      <tr><th>版本</th><th>大小</th><th>SHA-256</th><th>发布者</th><th>时间</th><th></th></tr>
      <?php foreach ($vers as $v): ?>
      <tr>
        <td><b><?= h($v['version']) ?></b></td>
        <td><?= h(human_size($v['size'] ?? 0)) ?></td>
        <td><code style="font-size:12px"><?= h(substr((string)($v['sha256'] ?? ''), 0, 12)) ?>…</code></td>
        <td><?= h($v['owner'] ?? '—') ?></td>
        <td><?= h(substr((string)($v['created_at'] ?? ''), 0, 10)) ?></td>
        <td><a class="btn small" href="lux.php?action=download&amp;name=<?= urlencode($name) ?>&amp;version=<?= urlencode($v['version']) ?>">下载</a></td>
      </tr>
      <?php endforeach; ?>
    </table>
    <?php if (!empty($latestP['deps'])): ?>
      <h3 style="margin:18px 0 4px;font-size:16px">依赖</h3>
      <ul><?php foreach ($latestP['deps'] as $d => $c): ?>
        <li><a href="?p=pkg&amp;name=<?= urlencode($d) ?>"><?= h($d) ?></a> <span class="pill"><?= h($c) ?></span></li>
      <?php endforeach; ?></ul>
    <?php endif; ?>
    <?php if ($user && (($latestP['owner'] ?? '') === $user['name'] || !empty($user['admin']))): ?>
      <div style="margin-top:18px;display:flex;gap:10px">
        <a class="btn" href="?p=edit&amp;name=<?= urlencode($name) ?>&amp;version=<?= urlencode($latestP['version']) ?>">编辑元数据</a>
        <form method="post" onsubmit="return confirm('删除整个包 <?= h($name) ?>？')">
          <input type="hidden" name="csrf" value="<?= h(csrf()) ?>">
          <input type="hidden" name="do" value="delete">
          <input type="hidden" name="name" value="<?= h($name) ?>">
          <button class="btn danger" type="submit">删除整个包</button>
        </form>
      </div>
    <?php endif; ?>
  </div>
  <?php break;

case 'login': ?>
  <div class="panel" style="max-width:440px;margin:40px auto">
    <h2 style="margin-top:0">登录</h2>
    <form class="form" method="post">
      <input type="hidden" name="csrf" value="<?= h(csrf()) ?>">
      <input type="hidden" name="do" value="login">
      <div><label>用户名</label><input name="username" required autofocus></div>
      <div><label>密码</label><input type="password" name="password" required></div>
      <button class="btn primary" type="submit">登录</button>
      <div class="hint">还没有账号？<a href="?p=register">立即注册</a></div>
    </form>
  </div>
  <?php break;

case 'register': ?>
  <div class="panel" style="max-width:440px;margin:40px auto">
    <h2 style="margin-top:0">注册</h2>
    <form class="form" method="post">
      <input type="hidden" name="csrf" value="<?= h(csrf()) ?>">
      <input type="hidden" name="do" value="register">
      <div><label>用户名（3~32 位字母 / 数字 / 下划线）</label><input name="username" required autofocus></div>
      <div><label>密码（至少 6 位）</label><input type="password" name="password" required></div>
      <div><label>邮箱（可选）</label><input type="email" name="email"></div>
      <button class="btn primary" type="submit">注册并登录</button>
      <div class="hint">已有账号？<a href="?p=login">去登录</a></div>
    </form>
  </div>
  <?php break;

case 'dashboard':
  if (!$user) { echo '<div class="empty">请先<a href="?p=login">登录</a>。</div>'; break; }
  $mine = array_values(array_filter($all, fn($p) => ($p['owner'] ?? '') === $user['name'])); ?>
  <h2 class="section">我的包（<?= h($user['name']) ?>）</h2>
  <p><a class="btn primary" href="?p=upload">+ 上传新包</a></p>
  <?php if (!$mine): ?>
    <div class="empty">你还没有发布过包。</div>
  <?php else: ?>
    <table>
      <tr><th>包名</th><th>版本</th><th>大小</th><th>更新时间</th><th>操作</th></tr>
      <?php foreach ($mine as $p): ?>
      <tr>
        <td><a href="?p=pkg&amp;name=<?= urlencode($p['name']) ?>"><b><?= h($p['name']) ?></b></a></td>
        <td><span class="pill v">v<?= h($p['version']) ?></span></td>
        <td><?= h(human_size($p['size'] ?? 0)) ?></td>
        <td><?= h(substr((string)($p['updated_at'] ?? $p['created_at'] ?? ''), 0, 16)) ?></td>
        <td style="white-space:nowrap">
          <a class="btn small" href="?p=edit&amp;name=<?= urlencode($p['name']) ?>&amp;version=<?= urlencode($p['version']) ?>">编辑</a>
          <form method="post" style="display:inline" onsubmit="return confirm('删除 <?= h($p['name']) ?>@<?= h($p['version']) ?>？')">
            <input type="hidden" name="csrf" value="<?= h(csrf()) ?>">
            <input type="hidden" name="do" value="delete">
            <input type="hidden" name="name" value="<?= h($p['name']) ?>">
            <input type="hidden" name="version" value="<?= h($p['version']) ?>">
            <button class="btn danger small" type="submit">删除</button>
          </form>
        </td>
      </tr>
      <?php endforeach; ?>
    </table>
  <?php endif;
  break;

case 'upload':
  if (!$user) { echo '<div class="empty">请先<a href="?p=login">登录</a>后再上传。</div>'; break; } ?>
  <div class="breadcrumb"><a href="?p=dashboard">我的包</a> / 上传</div>
  <div class="panel">
    <h2 style="margin-top:0">上传新包 <span class="pill">需登录</span></h2>
    <p class="hint">先在本地用 <code>luxc publish</code> 或 <code>tar -czf pkg.tar.gz -C 包目录 .</code>
       打出 <code>.tar.gz</code> 归档，再连同元数据一起上传。归档需包含 <code>lux.json</code>。</p>
    <form class="form" method="post" enctype="multipart/form-data">
      <input type="hidden" name="csrf" value="<?= h(csrf()) ?>">
      <input type="hidden" name="do" value="upload">
      <div class="row">
        <div><label>包名 *</label><input name="name" required placeholder="mypkg"></div>
        <div><label>版本 *</label><input name="version" required placeholder="0.1.0"></div>
      </div>
      <div><label>一句话简介</label><input name="summary" placeholder="这个包是做什么的"></div>
      <div><label>详细说明</label><textarea name="description"></textarea></div>
      <div class="row">
        <div><label>入口文件</label><input name="main" value="lib.lux"></div>
        <div><label>Lux 版本要求</label><input name="lux" placeholder=">=0.8.0"></div>
      </div>
      <div class="row">
        <div><label>许可</label><input name="license" placeholder="MIT"></div>
        <div><label>主页</label><input name="homepage" placeholder="https://…"></div>
      </div>
      <div><label>源文件（逗号或换行分隔）</label><input name="files" placeholder="lib.lux"></div>
      <div><label>标签</label><input name="tags" placeholder="math, utils"></div>
      <div><label>作者</label><input name="authors" placeholder="你的名字"></div>
      <div><label>依赖（每行 name 或 name@约束）</label><textarea name="deps" placeholder="mathx@^0.1.0"></textarea></div>
      <div><label>归档文件（.tar.gz / .tgz）*</label><input type="file" name="archive" accept=".gz,.tgz,application/gzip" required></div>
      <button class="btn primary" type="submit">发布</button>
    </form>
  </div>
  <?php break;

case 'edit':
  if (!$user) { echo '<div class="empty">请先<a href="?p=login">登录</a>。</div>'; break; }
  $name = (string)($_GET['name'] ?? '');
  $version = (string)($_GET['version'] ?? '');
  $m = null;
  if (valid_component($name) && valid_version($version) && is_file(pkg_meta_file($name, $version))) {
    $m = json_decode((string)@file_get_contents(pkg_meta_file($name, $version)), true);
  }
  if (!$m) { echo '<div class="empty">包或版本不存在。</div>'; break; }
  if (($m['owner'] ?? '') !== $user['name'] && empty($user['admin'])) {
    echo '<div class="empty">只能修改自己发布的包。</div>'; break;
  } ?>
  <div class="breadcrumb"><a href="?p=dashboard">我的包</a> / 编辑 <?= h($name) ?>@<?= h($version) ?></div>
  <div class="panel">
    <h2 style="margin-top:0">编辑 <?= h($name) ?> <span class="pill v">v<?= h($version) ?></span></h2>
    <form class="form" method="post">
      <input type="hidden" name="csrf" value="<?= h(csrf()) ?>">
      <input type="hidden" name="do" value="edit">
      <input type="hidden" name="name" value="<?= h($name) ?>">
      <input type="hidden" name="version" value="<?= h($version) ?>">
      <div><label>一句话简介</label><input name="summary" value="<?= h($m['summary'] ?? '') ?>"></div>
      <div><label>详细说明</label><textarea name="description"><?= h($m['description'] ?? '') ?></textarea></div>
      <div class="row">
        <div><label>入口文件</label><input name="main" value="<?= h($m['main'] ?? '') ?>"></div>
        <div><label>Lux 版本要求</label><input name="lux" value="<?= h($m['lux'] ?? '') ?>"></div>
      </div>
      <div class="row">
        <div><label>许可</label><input name="license" value="<?= h($m['license'] ?? '') ?>"></div>
        <div><label>主页</label><input name="homepage" value="<?= h($m['homepage'] ?? '') ?>"></div>
      </div>
      <div><label>源文件</label><input name="files" value="<?= h(implode(', ', $m['files'] ?? [])) ?>"></div>
      <div><label>标签</label><input name="tags" value="<?= h(implode(', ', $m['tags'] ?? [])) ?>"></div>
      <div><label>作者</label><input name="authors" value="<?= h(implode(', ', $m['authors'] ?? [])) ?>"></div>
      <div><label>依赖（每行 name 或 name@约束）</label><textarea name="deps"><?= h(deps_to_text($m['deps'] ?? [])) ?></textarea></div>
      <button class="btn primary" type="submit">保存</button>
    </form>
  </div>
  <?php break;

case 'downloads': ?>
  <h2 class="section">源码下载</h2>
  <div class="panel">
    <h3 style="margin-top:0">Lux 编译器源码</h3>
    <p class="hint">当前版本与历史版本（含 C++ 源码、文档、示例、测试）。解压后 <code>make</code> 即可构建。</p>
    <?php
    $luxDl = array_values(array_filter($downloads, fn($d) => ($d['kind'] ?? '') === 'lux'));
    usort($luxDl, fn($a, $b) => version_compare($b['version'] ?? '0', $a['version'] ?? '0'));
    if (!$luxDl): ?><div class="empty">暂无。</div><?php else: foreach ($luxDl as $d): ?>
      <div class="dl">
        <div>
          <div class="t"><?= h($d['title'] ?? $d['file']) ?></div>
          <div class="d"><?= h($d['desc'] ?? '') ?><?= !empty($d['size']) ? ' · ' . h(human_size($d['size'])) : '' ?></div>
        </div>
        <a class="btn primary small" href="downloads/<?= h(rawurlencode($d['file'])) ?>" download>下载</a>
      </div>
    <?php endforeach; endif; ?>
  </div>
  <div class="panel">
    <h3 style="margin-top:0">包管理系统源码</h3>
    <p class="hint">本注册表的完整 PHP 源码（API + 网页 + 部署说明），可直接部署到自己的空间。</p>
    <?php
    $srvDl = array_values(array_filter($downloads, fn($d) => ($d['kind'] ?? '') === 'server'));
    if (!$srvDl): ?><div class="empty">暂无。</div><?php else: foreach ($srvDl as $d): ?>
      <div class="dl">
        <div>
          <div class="t"><?= h($d['title'] ?? $d['file']) ?></div>
          <div class="d"><?= h($d['desc'] ?? '') ?><?= !empty($d['size']) ? ' · ' . h(human_size($d['size'])) : '' ?></div>
        </div>
        <a class="btn primary small" href="downloads/<?= h(rawurlencode($d['file'])) ?>" download>下载</a>
      </div>
    <?php endforeach; endif; ?>
  </div>
  <?php
  $other = array_values(array_filter($downloads, fn($d) => !in_array($d['kind'] ?? '', ['lux', 'server'], true)));
  if ($other): ?>
  <div class="panel">
    <h3 style="margin-top:0">其他</h3>
    <?php foreach ($other as $d): ?>
      <div class="dl"><div><div class="t"><?= h($d['title'] ?? $d['file']) ?></div>
        <div class="d"><?= h($d['desc'] ?? '') ?></div></div>
        <a class="btn small" href="downloads/<?= h(rawurlencode($d['file'])) ?>" download>下载</a></div>
    <?php endforeach; ?>
  </div>
  <?php endif;
  break;

case 'about': ?>
  <h2 class="section">关于</h2>
  <div class="panel">
    <p>这是 Lux 的官方包注册表，为 <code>luxc install</code> 提供索引与归档，同时提供网页端的
       注册 / 登录 / 上传 / 修改 / 删除 / 浏览 / 下载。</p>
    <h3>命令行</h3>
    <pre class="code">luxc login                  # 登录并保存令牌
luxc publish ./mypkg        # 发布到注册表（需登录）
luxc unpublish mypkg        # 删除自己发布的包
luxc install mypkg          # 安装
luxc search math            # 搜索
luxc upgrade                # 升级全部</pre>
    <h3>接口</h3>
    <table>
      <tr><th>动作</th><th>说明</th></tr>
      <tr><td><code>?action=index</code></td><td>全部包的全部版本</td></tr>
      <tr><td><code>?action=search&amp;q=</code></td><td>搜索（最新版本）</td></tr>
      <tr><td><code>?action=info&amp;name=</code></td><td>单包全部版本</td></tr>
      <tr><td><code>?action=download&amp;name=&amp;version=</code></td><td>下载归档</td></tr>
      <tr><td><code>?action=register / login</code></td><td>注册 / 登录（返回令牌）</td></tr>
      <tr><td><code>?action=publish / edit / delete</code></td><td>发布 / 修改 / 删除（需令牌）</td></tr>
      <tr><td><code>?action=health</code></td><td>健康检查</td></tr>
    </table>
    <p class="hint">外部注册表只要返回相同 JSON 结构即可被 luxc 使用：
      <code>luxc registry &lt;URL&gt;</code> 或 <code>LUX_REGISTRY</code>。</p>
  </div>
  <?php break;

default:
  echo '<div class="empty">页面不存在。<a href="?p=home">回首页</a></div>';
}

render_foot();
