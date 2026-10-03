<?php
// =============================================================================
//  index.php : Lux 包注册表 —— 网页界面（1.2.0）
//
//  在线登录 / 注册 / 浏览 / 搜索 / 上传 / 修改 / 删除自己的包；
//  也可查看与下载别人的包。上传需要账号，且只能改删自己发布的包。
//
//  上传支持：直接传 .tar.gz / 上传具体文件由站点打包 / 更新包在旧版本上叠加。
//  另提供：包管理系统源码下载、Lux 各版本源码下载、意见反馈。
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
    // 表单超过 post_max_size 时 PHP 会清空 POST/FILES，给出可读提示
    if (empty($_POST) && empty($_FILES) && (int)($_SERVER['CONTENT_LENGTH'] ?? 0) > 0) {
        flash('上传内容超过服务器限制（post_max_size = ' . ini_get('post_max_size') . '）', 'err');
        redirect('?p=upload');
    }
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

    // 意见反馈：任何人可提交（免登录）
    if ($do === 'feedback') {
        $err = '';
        $item = feedback_add((string)($_POST['type'] ?? ''),
                             (string)($_POST['content'] ?? ''),
                             (string)($_POST['contact'] ?? ''),
                             $user['name'] ?? '', $err);
        if (!$item) { flash($err, 'err'); redirect('?p=feedback'); }
        flash('感谢反馈！已收到，编号 ' . $item['id'] . '。');
        redirect('?p=feedback');
    }
    if ($do === 'feedback_status' || $do === 'feedback_delete') {
        if (!$user || empty($user['admin'])) {
            flash('只有管理员可以处理反馈', 'err');
            redirect('?p=feedback');
        }
        $id = (string)($_POST['id'] ?? '');
        if ($do === 'feedback_status') {
            $status = ((string)($_POST['status'] ?? 'done')) === 'open' ? 'open' : 'done';
            feedback_set_status($id, $status);
            flash($status === 'done' ? '已标记为已处理' : '已重新打开');
        } else {
            feedback_delete($id);
            flash('已删除该条反馈');
        }
        redirect('?p=feedback');
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
        $base = trim((string)($_POST['base_version'] ?? ''));
        $provide = (string)($_POST['provide'] ?? 'archive');
        $err = '';
        if ($provide === 'files') {
            $saved = pkg_store_files($_FILES['files'] ?? [], $meta, $user['name'],
                                     !empty($user['admin']), $base, $err);
        } else {
            $saved = pkg_store_upload($_FILES['archive'] ?? [], $meta, $user['name'],
                                      !empty($user['admin']), $err,
                                      $base !== '' ? $base : null);
        }
        if (!$saved) { flash($err, 'err'); redirect('?p=upload&name=' . urlencode($meta['name'])); }
        flash(($base !== '' ? '已发布更新包 ' : '已发布 ') . $saved['name'] . '@' . $saved['version']);
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
        'feedback' => '意见反馈', 'about' => '关于',
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
.seg{display:flex;flex-wrap:wrap;gap:8px}
.seg input{position:absolute;opacity:0;width:0;height:0}
.seg label{display:inline-flex;align-items:center;gap:7px;padding:8px 14px;border:1px solid var(--line);
  border-radius:10px;background:var(--panel2);font-size:13.5px;color:var(--muted);cursor:pointer;
  transition:border-color .15s,color .15s,background .15s}
.seg input:checked+label{border-color:var(--accent);color:var(--fg);background:rgba(124,92,255,.14);font-weight:600}
.seg input:focus-visible+label{outline:2px solid var(--accent2);outline-offset:2px}
.hidden{display:none!important}
.drop{border:1.5px dashed var(--line);border-radius:12px;padding:26px 18px;text-align:center;
  color:var(--muted);background:var(--panel2);cursor:pointer;transition:border-color .15s,color .15s}
.drop.over,.drop:hover{border-color:var(--accent);color:var(--fg)}
.drop input[type=file]{display:none}
.filelist{margin-top:10px;max-height:230px;overflow:auto;border:1px solid var(--line);border-radius:10px}
.filelist .row{display:flex;justify-content:space-between;gap:10px;padding:7px 12px;
  border-bottom:1px solid var(--line);font-size:13px}
.filelist .row:last-child{border-bottom:0}
.filelist .row .n{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.filelist .row .s{color:var(--muted);white-space:nowrap}
.fb{border:1px solid var(--line);border-radius:12px;padding:14px 16px;margin:10px 0;background:var(--panel2)}
.fb .head{display:flex;gap:10px;align-items:center;font-size:13px;color:var(--muted);flex-wrap:wrap}
.fb .body{margin-top:8px;white-space:pre-wrap;word-break:break-word}
.badge{font-size:12px;border-radius:999px;padding:2px 9px;border:1px solid var(--line)}
.badge.open{color:var(--gold);border-color:#4a4020}
.badge.done{color:var(--ok);border-color:#214a33}
.badge.type{color:var(--accent2);border-color:#274a66}
@media(max-width:640px){.form .row{grid-template-columns:1fr}.hero h1{font-size:30px}
  .seg label{flex:1 1 100%;justify-content:center}}
</style>
</head>
<body>
<header class="top"><div class="wrap nav">
  <a class="brand" href="?p=home"><span class="dot">λ</span> Lux 包注册表</a>
  <a class="link" href="?p=browse">浏览</a>
  <a class="link" href="?p=downloads">下载</a>
  <a class="link" href="?p=feedback">反馈</a>
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
    API：<code>lux.php</code> · <a href="?p=downloads">源码下载</a> ·
    <a href="?p=feedback">意见反馈</a>
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

function render_feedback_item(array $f, bool $admin): void {
    $done = ($f['status'] ?? 'open') === 'done'; ?>
  <div class="fb">
    <div class="head">
      <span class="badge type"><?= h($f['type'] ?? '其他') ?></span>
      <span class="badge <?= $done ? 'done' : 'open' ?>"><?= $done ? '已处理' : '待处理' ?></span>
      <span><?= h(substr((string)($f['time'] ?? ''), 0, 16)) ?></span>
      <span><?= h(!empty($f['user']) ? '@' . $f['user'] : '匿名') ?></span>
      <?php if (!empty($f['id'])): ?><code style="font-size:12px">#<?= h($f['id']) ?></code><?php endif; ?>
    </div>
    <div class="body"><?= h($f['content'] ?? '') ?></div>
    <?php if (!empty($f['contact'])): ?>
      <div class="hint">联系方式：<?= h($f['contact']) ?></div>
    <?php endif; ?>
    <?php if ($admin): ?>
      <div style="margin-top:10px;display:flex;gap:8px">
        <form method="post" style="display:inline">
          <input type="hidden" name="csrf" value="<?= h(csrf()) ?>">
          <input type="hidden" name="do" value="feedback_status">
          <input type="hidden" name="id" value="<?= h($f['id'] ?? '') ?>">
          <input type="hidden" name="status" value="<?= $done ? 'open' : 'done' ?>">
          <button class="btn small" type="submit"><?= $done ? '重新打开' : '标记已处理' ?></button>
        </form>
        <form method="post" style="display:inline" onsubmit="return confirm('删除这条反馈？')">
          <input type="hidden" name="csrf" value="<?= h(csrf()) ?>">
          <input type="hidden" name="do" value="feedback_delete">
          <input type="hidden" name="id" value="<?= h($f['id'] ?? '') ?>">
          <button class="btn danger small" type="submit">删除</button>
        </form>
      </div>
    <?php endif; ?>
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
      <div style="margin-top:18px;display:flex;gap:10px;flex-wrap:wrap">
        <a class="btn primary" href="?p=upload&amp;release=update&amp;name=<?= urlencode($name) ?>&amp;base=<?= urlencode($latestP['version']) ?>">发布更新包</a>
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
  <p>
    <a class="btn primary" href="?p=upload">+ 上传新包</a>
    <?php if (!empty($user['admin'])): ?>
      <a class="btn" href="?p=feedback">查看 / 处理反馈</a>
    <?php endif; ?>
  </p>
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
  if (!$user) { echo '<div class="empty">请先<a href="?p=login">登录</a>后再上传。</div>'; break; }
  $prefName = (string)($_GET['name'] ?? '');
  $prefBase = (string)($_GET['base'] ?? '');
  $prefRelease = (string)($_GET['release'] ?? 'full'); ?>
  <div class="breadcrumb"><a href="?p=dashboard">我的包</a> / 上传</div>
  <div class="panel">
    <h2 style="margin-top:0">上传 / 更新包 <span class="pill">需登录</span></h2>
    <p class="hint">两种提供方式：<b>直接上传 .tar.gz 压缩包</b>（本地用 <code>luxc publish</code> 或
       <code>tar -czf pkg.tar.gz -C 包目录 .</code> 打好），或<b>上传具体文件</b>由站点自动打包。
       <b>更新包</b>只需提供改动 / 新增的文件，服务端会在所选基础版本上叠加生成新版本。
       归档上限 16 MB，单个文件 8 MB。</p>
    <form class="form" method="post" enctype="multipart/form-data" id="upload-form">
      <input type="hidden" name="csrf" value="<?= h(csrf()) ?>">
      <input type="hidden" name="do" value="upload">
      <div class="row">
        <div><label>包名 *</label><input name="name" id="f-name" required value="<?= h($prefName) ?>" placeholder="mypkg"></div>
        <div><label>版本 *</label><input name="version" required placeholder="0.1.0"></div>
      </div>

      <div>
        <label>发布类型</label>
        <div class="seg">
          <input type="radio" id="rel-full" name="release" value="full" <?= $prefRelease !== 'update' ? 'checked' : '' ?>>
          <label for="rel-full">完整包</label>
          <input type="radio" id="rel-update" name="release" value="update" <?= $prefRelease === 'update' ? 'checked' : '' ?>>
          <label for="rel-update">更新包（在已有版本上叠加）</label>
        </div>
      </div>
      <div id="base-row" class="<?= $prefRelease === 'update' ? '' : 'hidden' ?>">
        <label>基础版本 *</label>
        <select name="base_version" id="f-base">
          <option value="<?= h($prefBase) ?>"><?= $prefBase !== '' ? h($prefBase) : '（输入包名后自动加载版本）' ?></option>
        </select>
        <div class="hint">合并规则：基础版本的文件保持不变，本次上传的同名文件覆盖、新文件追加。
           请确认包名已发布过该版本。</div>
      </div>

      <div>
        <label>提供方式</label>
        <div class="seg">
          <input type="radio" id="prov-archive" name="provide" value="archive" checked>
          <label for="prov-archive">直接上传 .tar.gz</label>
          <input type="radio" id="prov-files" name="provide" value="files">
          <label for="prov-files">上传文件，站点打包</label>
        </div>
      </div>
      <div id="provide-archive">
        <label>归档文件（.tar.gz / .tgz）*</label>
        <input type="file" name="archive" id="f-archive" accept=".gz,.tgz,application/gzip">
        <div class="hint">更新包模式下，这里上传的也应该是只含改动文件的 .tar.gz。</div>
      </div>
      <div id="provide-files" class="hidden">
        <label>源文件 *</label>
        <div class="drop" id="drop">
          <input type="file" id="f-files" name="files[]" multiple>
          <div>把文件拖到这里，或 <a href="#" id="pick">点击选择文件</a></div>
          <div class="hint">可勾选「整个文件夹」或拖入文件夹（Chromium 系浏览器）批量上传；
             站点会自动生成包含 <code>lux.json</code> 的规范归档。</div>
        </div>
        <label style="display:flex;align-items:center;gap:8px;margin-top:10px;color:var(--fg);cursor:pointer">
          <input type="checkbox" id="dir-toggle" style="width:auto"> 选择整个文件夹
        </label>
        <div id="filelist" class="filelist hidden"></div>
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
      <div><label>源文件（逗号或换行分隔，留空时按上传文件自动填写）</label><input name="files" placeholder="lib.lux"></div>
      <div><label>标签</label><input name="tags" placeholder="math, utils"></div>
      <div><label>作者</label><input name="authors" placeholder="你的名字"></div>
      <div><label>依赖（每行 name 或 name@约束）</label><textarea name="deps" placeholder="mathx@^0.1.0"></textarea></div>
      <button class="btn primary" type="submit">发布</button>
    </form>
  </div>
  <script>
  (function(){
    var rel = document.querySelectorAll('input[name=release]');
    var prov = document.querySelectorAll('input[name=provide]');
    var nameInput = document.getElementById('f-name');
    var baseRow = document.getElementById('base-row');
    var baseSel = document.getElementById('f-base');
    var prefBase = <?= json_encode($prefBase, JSON_UNESCAPED_UNICODE) ?>;
    function checkedVal(list){ for (var i=0;i<list.length;i++) if (list[i].checked) return list[i].value; return ''; }
    function loadVersions(){
      var n = nameInput.value.trim();
      if (!n) return;
      fetch('lux.php?action=info&name=' + encodeURIComponent(n))
        .then(function(r){ return r.json(); })
        .then(function(j){
          if (!j || !j.versions) return;
          baseSel.innerHTML = '';
          j.versions.forEach(function(v){
            var o = document.createElement('option');
            o.value = v.version;
            o.textContent = 'v' + v.version + (v.owner ? '（@' + v.owner + '）' : '');
            if (prefBase && v.version === prefBase) o.selected = true;
            baseSel.appendChild(o);
          });
        }).catch(function(){});
    }
    function showBase(){
      var upd = checkedVal(rel) === 'update';
      baseRow.classList.toggle('hidden', !upd);
      if (upd) loadVersions();
    }
    rel.forEach(function(r){ r.addEventListener('change', showBase); });
    nameInput.addEventListener('change', function(){ if (checkedVal(rel)==='update') loadVersions(); });
    prov.forEach(function(r){ r.addEventListener('change', function(){
      var files = checkedVal(prov) === 'files';
      document.getElementById('provide-archive').classList.toggle('hidden', files);
      document.getElementById('provide-files').classList.toggle('hidden', !files);
    }); });

    var filesInput = document.getElementById('f-files');
    var drop = document.getElementById('drop');
    var list = document.getElementById('filelist');
    var dirToggle = document.getElementById('dir-toggle');
    function fmt(n){ if (n<1024) return n+' B'; if (n<1048576) return (n/1024).toFixed(1)+' KB'; return (n/1048576).toFixed(1)+' MB'; }
    function render(){
      var fs = filesInput.files || [];
      list.innerHTML = '';
      var total = 0;
      for (var i=0;i<fs.length;i++){
        total += fs[i].size;
        var row = document.createElement('div'); row.className='row';
        var n = document.createElement('span'); n.className='n';
        n.textContent = fs[i].webkitRelativePath || fs[i].name;
        var s = document.createElement('span'); s.className='s'; s.textContent = fmt(fs[i].size);
        row.appendChild(n); row.appendChild(s); list.appendChild(row);
      }
      if (fs.length){
        var t = document.createElement('div'); t.className='row';
        var tn = document.createElement('span'); tn.className='n';
        tn.innerHTML = '<b>共 ' + fs.length + ' 个文件</b>' + (total > 16777216 ? ' — 超过 16 MB 限制' : '');
        var ts = document.createElement('span'); ts.className='s'; ts.textContent = fmt(total);
        t.appendChild(tn); t.appendChild(ts); list.appendChild(t);
        list.classList.remove('hidden');
      } else list.classList.add('hidden');
    }
    drop.addEventListener('click', function(e){ e.preventDefault(); filesInput.click(); });
    drop.addEventListener('dragover', function(e){ e.preventDefault(); drop.classList.add('over'); });
    drop.addEventListener('dragleave', function(){ drop.classList.remove('over'); });
    drop.addEventListener('drop', function(e){
      e.preventDefault(); drop.classList.remove('over');
      if (e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files.length){
        filesInput.files = e.dataTransfer.files; render();
      }
    });
    filesInput.addEventListener('change', render);
    dirToggle.addEventListener('change', function(){
      filesInput.webkitdirectory = dirToggle.checked;
      filesInput.value = ''; render();
    });
    showBase();
  })();
  </script>
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
  $updDl = array_values(array_filter($downloads, fn($d) => ($d['kind'] ?? '') === 'update'));
  if ($updDl): ?>
  <div class="panel">
    <h3 style="margin-top:0">更新包</h3>
    <p class="hint">从旧版本升级到新版本的增量包，只包含变化的文件。</p>
    <?php foreach ($updDl as $d): ?>
      <div class="dl">
        <div>
          <div class="t"><?= h($d['title'] ?? $d['file']) ?></div>
          <div class="d"><?= h($d['desc'] ?? '') ?><?= !empty($d['size']) ? ' · ' . h(human_size($d['size'])) : '' ?></div>
        </div>
        <a class="btn primary small" href="downloads/<?= h(rawurlencode($d['file'])) ?>" download>下载</a>
      </div>
    <?php endforeach; ?>
  </div>
  <?php endif;
  $other = array_values(array_filter($downloads, fn($d) => !in_array($d['kind'] ?? '', ['lux', 'server', 'update'], true)));
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

case 'feedback':
  $isAdmin = $user && !empty($user['admin']);
  $myFb = $user
      ? array_values(array_filter(feedback_list(), fn($f) => ($f['user'] ?? '') === $user['name']))
      : [];
  $allFb = $isAdmin ? feedback_list() : []; ?>
  <h2 class="section">意见反馈</h2>
  <div class="panel" style="max-width:760px;margin:0 auto">
    <h3 style="margin-top:0">提交反馈</h3>
    <p class="hint">使用中遇到的问题、想要的功能、对注册表 / 包管理的建议都可以写在这里，无需登录。
       提交内容保存在服务端 <code>data/feedback.php</code>（带访问守卫）。</p>
    <form class="form" method="post">
      <input type="hidden" name="csrf" value="<?= h(csrf()) ?>">
      <input type="hidden" name="do" value="feedback">
      <div class="row">
        <div><label>类型</label>
          <select name="type">
            <?php foreach (feedback_types() as $t): ?>
              <option value="<?= h($t) ?>"><?= h($t) ?></option>
            <?php endforeach; ?>
          </select>
        </div>
        <div><label>联系方式（可选）</label><input name="contact"
          value="<?= h($user['email'] ?? '') ?>" placeholder="邮箱 / QQ / 其它"></div>
      </div>
      <div><label>内容 *</label><textarea name="content" required
        placeholder="请尽量描述清楚：做了什么、预期什么、实际怎样…"></textarea></div>
      <button class="btn primary" type="submit">提交反馈</button>
    </form>
  </div>
  <?php if ($myFb): ?>
    <h2 class="section">我的反馈（<?= count($myFb) ?>）</h2>
    <?php foreach ($myFb as $f) render_feedback_item($f, false); ?>
  <?php elseif ($user): ?>
    <div class="empty">你还没有提交过反馈。</div>
  <?php endif; ?>
  <?php if ($isAdmin): ?>
    <h2 class="section">全部反馈（管理员，<?= count($allFb) ?>）</h2>
    <?php if (!$allFb): ?><div class="empty">暂无反馈。</div>
    <?php else: foreach ($allFb as $f): render_feedback_item($f, true); endforeach; endif; ?>
  <?php endif; ?>
  <?php break;

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
