<?php
declare(strict_types=1);
// =============================================================================
//  index.php : lux.xfes.top 首页
//
//  单文件、无外部依赖。数据在可读取时动态生成（Lux 最新版本、下载列表、
//  包注册表包数），读取失败时自动回退到内置内容，页面始终可以正常显示。
// =============================================================================

function e(?string $s): string {
    return htmlspecialchars((string)$s, ENT_QUOTES, 'UTF-8');
}

$ROOT = __DIR__;

// ---- 读取下载清单（失败回退） ------------------------------------------------
$latest     = '1.1.0';
$luxDl      = [];
$serverDl   = [];
$dlFile     = $ROOT . '/lux/downloads/downloads.json';
if (is_readable($dlFile)) {
    $raw = @file_get_contents($dlFile);
    if ($raw !== false) {
        $j = json_decode($raw, true);
        if (is_array($j)) {
            if (!empty($j['version'])) $latest = (string)$j['version'];
            $all = (isset($j['downloads']) && is_array($j['downloads'])) ? $j['downloads'] : [];
            foreach ($all as $d) {
                $kind = (string)($d['kind'] ?? '');
                if ($kind === 'server') $serverDl[] = $d;
                elseif ($kind === 'lux') $luxDl[] = $d;
            }
            usort($luxDl, fn($a, $b) =>
                version_compare((string)($b['version'] ?? '0'), (string)($a['version'] ?? '0')));
        }
    }
}

// ---- 统计包注册表中的包数量 --------------------------------------------------
$pkgCount = 0;
$pkgDir   = $ROOT . '/lux/packages';
if (is_dir($pkgDir)) {
    $items = @scandir($pkgDir);
    if (is_array($items)) {
        foreach ($items as $d) {
            if ($d === '.' || $d === '..') continue;
            if (is_dir($pkgDir . '/' . $d)) $pkgCount++;
        }
    }
}

function fmt_size($bytes): string {
    $n = (float)$bytes;
    if ($n <= 0) return '—';
    $u = ['B', 'KB', 'MB', 'GB'];
    $i = 0;
    while ($n >= 1024 && $i < count($u) - 1) { $n /= 1024; $i++; }
    return ($i === 0 ? (string)(int)$n : number_format($n, 1)) . ' ' . $u[$i];
}

$year = date('Y');
?>
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="theme-color" content="#0b0e17">
<meta name="description" content="Lux —— 一门用 C++ 写的、编译到原生机器码的静态类型编程语言。双后端、零 libc 依赖，覆盖 x86-64 与 aarch64。">
<meta property="og:title" content="Lux · 静态类型原生编译语言">
<meta property="og:description" content="用 C++ 实现的静态类型编程语言，可直接编译为本机架构的 Linux ELF。">
<meta property="og:type" content="website">
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'%3E%3Crect width='64' height='64' rx='16' fill='%237c5cff'/%3E%3Ctext x='32' y='45' font-size='38' font-family='Georgia,serif' fill='%23fff' text-anchor='middle'%3E%CE%BB%3C/text%3E%3C/svg%3E">
<title>Lux · 静态类型原生编译语言</title>
<style>
  :root{
    --bg:#0b0e17; --bg2:#0d111d; --panel:#151a29; --panel2:#1c2333; --line:#2a3348;
    --fg:#e8ecf5; --muted:#96a0b8; --dim:#6c768e;
    --accent:#7c5cff; --accent2:#3aa0ff; --ok:#37d67a; --gold:#ffce5c; --err:#ff5c7a;
    --radius:16px;
    --shadow:0 18px 50px -20px rgba(0,0,0,.75);
  }
  *{box-sizing:border-box}
  html{-webkit-text-size-adjust:100%;scroll-behavior:smooth}
  body{
    margin:0;color:var(--fg);line-height:1.7;font-size:15px;
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC",
                "Hiragino Sans GB","Microsoft YaHei",sans-serif;
    background:
      radial-gradient(1100px 620px at 12% -14%, rgba(124,92,255,.20), transparent 60%),
      radial-gradient(950px 560px at 102% -4%, rgba(58,160,255,.14), transparent 58%),
      radial-gradient(800px 700px at 50% 118%, rgba(55,214,122,.07), transparent 60%),
      var(--bg);
    background-attachment:fixed;
  }
  a{color:var(--accent2);text-decoration:none}
  a:hover{text-decoration:underline}
  code,pre{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,"Liberation Mono",monospace}
  .wrap{max-width:1120px;margin:0 auto;padding:0 22px}

  /* ---------- 顶栏 ---------- */
  header.top{
    position:sticky;top:0;z-index:50;
    backdrop-filter:blur(12px);-webkit-backdrop-filter:blur(12px);
    background:rgba(11,14,23,.82);border-bottom:1px solid var(--line);
  }
  .nav{display:flex;align-items:center;gap:6px;height:62px}
  .brand{display:flex;align-items:center;gap:11px;font-weight:700;font-size:18px;
    letter-spacing:.02em;margin-right:16px;color:var(--fg)}
  .brand:hover{text-decoration:none}
  .brand .mark{width:30px;height:30px;border-radius:9px;display:grid;place-items:center;
    background:linear-gradient(135deg,var(--accent),var(--accent2));
    color:#fff;font:600 17px/1 Georgia,"Times New Roman",serif;
    box-shadow:0 6px 18px -6px rgba(124,92,255,.9)}
  .nav a.link{padding:8px 13px;border-radius:10px;color:var(--muted);font-size:14.5px;
    transition:background .15s,color .15s}
  .nav a.link:hover{background:var(--panel);color:var(--fg);text-decoration:none}
  .nav .spacer{flex:1}
  .btn{display:inline-flex;align-items:center;gap:8px;padding:9px 16px;border-radius:10px;
    border:1px solid var(--line);background:var(--panel);color:var(--fg);cursor:pointer;
    font-size:14px;line-height:1.2;font-weight:500;transition:background .15s,border-color .15s,transform .1s}
  .btn:hover{background:var(--panel2);text-decoration:none}
  .btn:active{transform:translateY(1px)}
  .btn.primary{background:linear-gradient(135deg,var(--accent),var(--accent2));
    border:0;color:#fff;font-weight:600;box-shadow:0 12px 30px -12px rgba(124,92,255,.95)}
  .btn.ghost{background:transparent}
  .btn.small{padding:7px 13px;font-size:13.5px}
  .btn svg{width:16px;height:16px;flex:none}
  @media (max-width:760px){ .nav a.link.hide-s{display:none} }

  /* ---------- 首屏 ---------- */
  .hero{padding:74px 0 30px;text-align:center;position:relative}
  .badge{display:inline-flex;align-items:center;gap:8px;padding:6px 14px;border-radius:999px;
    border:1px solid var(--line);background:rgba(21,26,41,.75);color:var(--muted);
    font-size:13px;letter-spacing:.03em;margin-bottom:24px}
  .badge .pulse{width:7px;height:7px;border-radius:50%;background:var(--ok);
    box-shadow:0 0 0 4px rgba(55,214,122,.16)}
  .hero h1{
    margin:0 0 16px;font-size:clamp(42px,8vw,74px);line-height:1.04;font-weight:750;
    letter-spacing:-.02em;
    background:linear-gradient(100deg,#eaf0ff 4%,#a9b8ff 42%,#6ee7dd 74%,#eaf0ff 100%);
    background-size:220% auto;
    -webkit-background-clip:text;background-clip:text;color:transparent;
    animation:shine 9s linear infinite;
  }
  @keyframes shine{to{background-position:220% center}}
  .hero .lead{margin:0 auto;max-width:660px;color:var(--muted);font-size:17px}
  .hero .lead b{color:var(--fg);font-weight:600}
  .cta{display:flex;flex-wrap:wrap;gap:12px;justify-content:center;margin-top:32px}
  .stats{display:flex;flex-wrap:wrap;gap:14px 34px;justify-content:center;margin-top:40px}
  .stat{text-align:center}
  .stat .v{font-size:24px;font-weight:700;color:var(--fg);letter-spacing:.01em}
  .stat .k{font-size:12.5px;color:var(--dim);letter-spacing:.06em;text-transform:uppercase}

  /* ---------- 终端 / 代码 ---------- */
  .terminal{
    margin:46px auto 0;max-width:760px;text-align:left;
    background:linear-gradient(180deg,#101524,#0c1019);
    border:1px solid var(--line);border-radius:var(--radius);overflow:hidden;box-shadow:var(--shadow);
  }
  .terminal .bar{display:flex;align-items:center;gap:8px;padding:11px 15px;
    border-bottom:1px solid var(--line);background:rgba(255,255,255,.02)}
  .terminal .bar i{width:11px;height:11px;border-radius:50%;display:block;opacity:.85}
  .terminal .bar .r{background:#ff5f57}.terminal .bar .y{background:#ffbd2e}.terminal .bar .g{background:#28c840}
  .terminal .bar span{margin-left:8px;color:var(--dim);font-size:12.5px;letter-spacing:.05em}
  .terminal pre{margin:0;padding:20px 22px;overflow:auto;font-size:13.5px;line-height:1.75}
  .c-key{color:#c6a4ff}.c-fn{color:#7fd3ff}.c-type{color:#6ee7dd}.c-num{color:#ffce5c}
  .c-str{color:#9ff0bf}.c-com{color:#5d6780;font-style:italic}.c-prompt{color:var(--ok)}

  /* ---------- 通用小节 ---------- */
  section{padding:64px 0 8px}
  .sec-head{margin-bottom:30px}
  .sec-head .eyebrow{color:var(--accent2);font-size:12.5px;font-weight:600;
    letter-spacing:.16em;text-transform:uppercase}
  .sec-head h2{font-size:clamp(24px,4vw,32px);margin:8px 0 10px;letter-spacing:-.01em}
  .sec-head p{margin:0;color:var(--muted);max-width:720px}

  .grid{display:grid;gap:16px}
  .cols-3{grid-template-columns:repeat(auto-fill,minmax(300px,1fr))}
  .cols-2{grid-template-columns:repeat(auto-fill,minmax(340px,1fr))}

  .card{
    position:relative;background:linear-gradient(180deg,rgba(28,35,51,.72),rgba(21,26,41,.72));
    border:1px solid var(--line);border-radius:var(--radius);padding:22px;
    transition:border-color .18s,transform .18s,box-shadow .18s;
  }
  .card:hover{border-color:#3f4d7d;transform:translateY(-3px);
    box-shadow:0 22px 44px -26px rgba(58,160,255,.55)}
  .card .ico{width:40px;height:40px;border-radius:11px;display:grid;place-items:center;
    background:rgba(124,92,255,.14);border:1px solid rgba(124,92,255,.32);margin-bottom:15px}
  .card .ico svg{width:20px;height:20px;stroke:var(--accent);fill:none;
    stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round}
  .card h3{margin:0 0 7px;font-size:16.5px;letter-spacing:.01em}
  .card p{margin:0;color:var(--muted);font-size:14px}

  /* ---------- 项目导航 ---------- */
  .proj{display:flex;flex-direction:column;height:100%}
  .proj .top{display:flex;align-items:center;gap:12px;margin-bottom:12px}
  .proj .emoji-none{width:38px;height:38px;border-radius:11px;display:grid;place-items:center;
    background:linear-gradient(135deg,rgba(124,92,255,.22),rgba(58,160,255,.18));
    border:1px solid var(--line);font:600 15px/1 ui-monospace,monospace;color:var(--accent2)}
  .proj h3{margin:0;font-size:17px}
  .proj .path{color:var(--dim);font-size:12.5px;font-family:ui-monospace,monospace;margin-top:2px}
  .proj p{flex:1}
  .proj .go{margin-top:16px;font-size:14px;font-weight:600;color:var(--accent2)}
  .proj:hover .go{color:#8fc4ff}
  .tags{display:flex;flex-wrap:wrap;gap:7px;margin-top:14px}
  .tag{font-size:12px;color:var(--muted);border:1px solid var(--line);
    background:var(--panel2);border-radius:999px;padding:2px 10px}

  /* ---------- 下载 ---------- */
  .dl-list{background:linear-gradient(180deg,rgba(28,35,51,.6),rgba(21,26,41,.6));
    border:1px solid var(--line);border-radius:var(--radius);overflow:hidden}
  .dl-row{display:flex;align-items:center;gap:16px;padding:15px 20px;
    border-bottom:1px solid var(--line);transition:background .15s}
  .dl-row:last-child{border-bottom:0}
  .dl-row:hover{background:rgba(58,160,255,.06)}
  .dl-row .badge-v{font-family:ui-monospace,monospace;font-size:12.5px;color:var(--gold);
    border:1px solid #4a4020;background:rgba(255,206,92,.07);border-radius:999px;padding:2px 11px;white-space:nowrap}
  .dl-row .meta{flex:1;min-width:0}
  .dl-row .meta .t{font-weight:600;font-size:14.5px}
  .dl-row .meta .d{color:var(--dim);font-size:13px;white-space:nowrap;overflow:hidden;
    text-overflow:ellipsis}
  .dl-row .sz{color:var(--dim);font-size:13px;font-variant-numeric:tabular-nums;white-space:nowrap}
  @media (max-width:560px){ .dl-row .sz{display:none} }

  /* ---------- 关于 ---------- */
  .about{display:grid;grid-template-columns:1.15fr .85fr;gap:20px}
  @media (max-width:820px){ .about{grid-template-columns:1fr} }
  .kv{display:grid;grid-template-columns:96px 1fr;gap:10px 16px;margin:0}
  .kv dt{color:var(--dim);font-size:13.5px}
  .kv dd{margin:0;font-size:14px;word-break:break-all}
  .links{display:flex;flex-wrap:wrap;gap:10px;margin-top:18px}

  footer{border-top:1px solid var(--line);margin-top:74px;
    color:var(--dim);font-size:13px;padding:30px 0}
  footer .inner{display:flex;flex-wrap:wrap;gap:10px 22px;align-items:center;justify-content:space-between}
  footer a{color:var(--muted)}
  footer a:hover{color:var(--fg)}
</style>
</head>
<body>

<header class="top">
  <div class="wrap nav">
    <a class="brand" href="/"><span class="mark">λ</span> Lux</a>
    <a class="link" href="#features">特性</a>
    <a class="link" href="#projects">项目</a>
    <a class="link" href="#downloads">下载</a>
    <a class="link hide-s" href="/lux/">包注册表</a>
    <a class="link hide-s" href="#about">关于</a>
    <span class="spacer"></span>
    <a class="btn small" href="/lux/">进入注册表</a>
  </div>
</header>

<main class="wrap">

  <!-- ===================== 首屏 ===================== -->
  <div class="hero">
    <div class="badge"><span class="pulse"></span> 当前版本 <?= e($latest) ?> · 双后端 · 无 libc 依赖</div>
    <h1>Lux</h1>
    <p class="lead">
      一门用 <b>C++ 编写的静态类型编程语言</b>，编译到原生机器码。
      语法现代简洁（<code>fn</code> / <code>let</code> / 类型后置），
      既可翻译成 C 交给 gcc 优化，也能<b>直出本机架构的 Linux ELF</b>，
      运行时系统全部由 Lux 自身实现；<b>ARC 引用计数自动回收内存</b>。
      语言、标准库、错误通道、构建与包管理均已在 <b>1.0 正式版</b>冻结；
      <b>1.1</b> 让具名函数成为可传递的值（<code>fn</code> 类型），并带来 <code>sort</code> / <code>map</code> / <code>filter</code>。
    </p>
    <div class="cta">
      <a class="btn primary" href="/lux/">
        <svg viewBox="0 0 24 24"><path d="M4 19V5m0 14h16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"/></svg>
        浏览包注册表
      </a>
      <a class="btn" href="#downloads">
        <svg viewBox="0 0 24 24"><path d="M12 3v12m0 0l-4-4m4 4l4-4M4 19h16" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></svg>
        下载源码
      </a>
      <a class="btn ghost" href="https://github.com/wssjh822" target="_blank" rel="noopener">
        <svg viewBox="0 0 24 24" fill="currentColor"><path d="M12 2C6.48 2 2 6.58 2 12.25c0 4.53 2.87 8.37 6.84 9.73.5.1.68-.22.68-.49l-.01-1.72c-2.78.62-3.37-1.37-3.37-1.37-.46-1.18-1.11-1.5-1.11-1.5-.91-.63.07-.62.07-.62 1 .07 1.53 1.06 1.53 1.06.9 1.57 2.34 1.12 2.91.85.09-.66.35-1.12.63-1.37-2.22-.26-4.56-1.14-4.56-5.06 0-1.12.39-2.03 1.03-2.75-.1-.26-.45-1.3.1-2.71 0 0 .84-.28 2.75 1.05a9.3 9.3 0 0 1 5 0c1.91-1.33 2.75-1.05 2.75-1.05.55 1.41.2 2.45.1 2.71.64.72 1.03 1.63 1.03 2.75 0 3.93-2.34 4.79-4.57 5.05.36.32.68.94.68 1.9l-.01 2.82c0 .27.18.6.69.49A10.28 10.28 0 0 0 22 12.25C22 6.58 17.52 2 12 2z"/></svg>
        GitHub
      </a>
    </div>

    <div class="stats">
      <div class="stat"><div class="v"><?= e($latest) ?></div><div class="k">最新版本</div></div>
      <div class="stat"><div class="v">C / 原生</div><div class="k">双编译后端</div></div>
      <div class="stat"><div class="v">x86-64 · arm64</div><div class="k">原生 ELF 目标</div></div>
      <div class="stat"><div class="v"><?= $pkgCount > 0 ? (int)$pkgCount : '—' ?></div><div class="k">注册表包数</div></div>
    </div>

    <div class="terminal">
      <div class="bar"><i class="r"></i><i class="y"></i><i class="g"></i><span>config.lux</span></div>
<pre><span class="c-com">// 错误通道 + 可选类型：读取或解析失败就返回 none</span>
<span class="c-key">import</span> <span class="c-str">"file"</span>;
<span class="c-key">import</span> <span class="c-str">"string"</span>;

<span class="c-key">fn</span> <span class="c-fn">load_port</span>(path: <span class="c-type">string</span>) -&gt; <span class="c-type">int</span>? {
    <span class="c-key">let</span> text = <span class="c-fn">read</span>(path)?;   <span class="c-com">// 失败则向上传播</span>
    <span class="c-key">return</span> <span class="c-fn">int</span>(text)?;
}

<span class="c-key">fn</span> <span class="c-fn">main</span>(argv: <span class="c-type">string</span>[]) {
    <span class="c-key">let</span> port = <span class="c-fn">load_port</span>(<span class="c-str">"app.conf"</span>) <span class="c-key">or</span> <span class="c-num">8080</span>;
    <span class="c-fn">print</span>(<span class="c-str">"listen on "</span> + <span class="c-fn">str</span>(port));
}

<span class="c-prompt">$</span> luxc build config.lux --native   <span class="c-com"># 直出本机 ELF</span></pre>
    </div>
  </div>

  <!-- ===================== 特性 ===================== -->
  <section id="features">
    <div class="sec-head">
      <div class="eyebrow">Language</div>
      <h2>为原生性能而设计的语言</h2>
      <p>从源码到可执行文件，每一步都尽量少依赖、少开销、少意外。</p>
    </div>
    <div class="grid cols-3">
      <div class="card">
        <div class="ico"><svg viewBox="0 0 24 24"><path d="M8 3H5a2 2 0 0 0-2 2v3m13-5h3a2 2 0 0 1 2 2v3M8 21H5a2 2 0 0 1-2-2v-3m13 5h3a2 2 0 0 0 2-2v-3"/><path d="M9 12h6"/></svg></div>
        <h3>双编译后端</h3>
        <p>C 后端把 Lux 翻译成 C，交给 gcc / clang 优化；原生后端（<code>--native</code>）直接发射 Linux ELF。</p>
      </div>
      <div class="card">
        <div class="ico"><svg viewBox="0 0 24 24"><path d="M13 2L4.5 13.5H11L10 22l8.5-11.5H12L13 2z"/></svg></div>
        <h3>零 libc 依赖</h3>
        <p>原生后端不链接 libc，运行时系统全部由 Lux 自身实现，产物干净、启动直接。</p>
      </div>
      <div class="card">
        <div class="ico"><svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="16" rx="2"/><path d="M3 10h18M8 4v16"/></svg></div>
        <h3>现代静态语法</h3>
        <p><code>fn</code> / <code>let</code> / 类型后置，配合 <code>struct</code> 结构体、<code>if</code> 表达式与原始字符串。</p>
      </div>
      <div class="card">
        <div class="ico"><svg viewBox="0 0 24 24"><path d="M12 3l9 5-9 5-9-5 9-5z"/><path d="M3 13l9 5 9-5"/></svg></div>
        <h3>可选类型与错误通道</h3>
        <p><code>T?</code> 可选类型、传播后缀 <code>expr?</code>、兜底表达式 <code>lhs or rhs</code>，panic 变体 <code>int!</code> / <code>read!</code>。</p>
      </div>
      <div class="card">
        <div class="ico"><svg viewBox="0 0 24 24"><path d="M4 7h16M4 12h16M4 17h10"/></svg></div>
        <h3>数组、切片与迭代</h3>
        <p>引用语义数组（带越界检查）、<code>for-in</code> 迭代、切片 <code>a[lo..hi]</code> 与 <code>split</code> / <code>join</code>。</p>
      </div>
      <div class="card">
        <div class="ico"><svg viewBox="0 0 24 24"><path d="M12 2v4m0 12v4M2 12h4m12 0h4M5 5l3 3m8 8l3 3m0-14l-3 3M8 16l-3 3"/></svg></div>
        <h3>真实机器码覆盖</h3>
        <p>原生后端同时覆盖 x86-64 与 aarch64，按宿主架构选择发射器，在 arm64 上也能直出可执行文件。</p>
      </div>
      <div class="card">
        <div class="ico"><svg viewBox="0 0 24 24"><path d="M21 12a9 9 0 1 1-2.64-6.36"/><path d="M21 3v6h-6"/></svg></div>
        <h3>自动内存管理</h3>
        <p>引用计数（ARC）在 <b>1.0 起两个后端默认开启</b>：临时对象在每条语句结束时归还，长驻进程不再只增不减；需要旧行为可用 <code>--no-arc</code>。</p>
      </div>
      <div class="card">
        <div class="ico"><svg viewBox="0 0 24 24"><path d="M14.7 6.3a4 4 0 0 0-5.4 5.4l-6 6a1.4 1.4 0 0 1-2-2l6-6a4 4 0 0 0 5.4-5.4l2.3 2.3-2 2-2.3-2.3z"/><path d="M15 9l6 6"/></svg></div>
        <h3>项目构建</h3>
        <p><code>luxc build</code> 读取 <code>LuxBuildFile</code>（类似 <code>make</code>）：一键构建 / 运行 / 测试 / 清理，支持 <code>backend</code> / <code>arc</code> / <code>testdir</code> 等配置。</p>
      </div>
      <div class="card">
        <div class="ico"><svg viewBox="0 0 24 24"><path d="M21 16V8l-9-5-9 5v8l9 5 9-5z"/><path d="M3.3 7.5L12 12l8.7-4.5M12 22V12"/></svg></div>
        <h3>在线包管理</h3>
        <p><code>luxc install</code> 从注册表下载并解析依赖，<code>login</code> / <code>publish</code> 带账号体系；自带 <code>mathx</code> / <code>strx</code> / <code>numx</code> / <code>arrx</code> / <code>jsonx</code>。</p>
      </div>
    </div>
  </section>

  <!-- ===================== 项目 ===================== -->
  <section id="projects">
    <div class="sec-head">
      <div class="eyebrow">Projects</div>
      <h2>本站的其他作品</h2>
      <p>除了 Lux 语言本身，站内还部署了几个可在浏览器中直接使用的工具与实验。</p>
    </div>
    <div class="grid cols-2">
      <a class="card proj" href="/lux/">
        <div class="top">
          <span class="emoji-none">λ</span>
          <div><h3>Lux 包注册表</h3><div class="path">/lux/</div></div>
        </div>
        <p>在线浏览、搜索、上传与下载 Lux 包，附带账号体系、令牌 API 与各版本源码下载。</p>
        <div class="tags"><span class="tag">PHP</span><span class="tag">无数据库</span><span class="tag">REST API</span></div>
        <div class="go">打开注册表 →</div>
      </a>

      <a class="card proj" href="/hex.html">
        <div class="top">
          <span class="emoji-none">0x</span>
          <div><h3>编码转换编辑器</h3><div class="path">/hex.html</div></div>
        </div>
        <p>在文本、十六进制与 Base64 之间即时互转，适合排查二进制、构造数据与快速编解码。</p>
        <div class="tags"><span class="tag">前端工具</span><span class="tag">无需上传</span></div>
        <div class="go">开始转换 →</div>
      </a>

      <a class="card proj" href="/piano.html">
        <div class="top">
          <span class="emoji-none">88</span>
          <div><h3>鎏金钢琴 · Gilded Grand</h3><div class="path">/piano.html</div></div>
        </div>
        <p>网页版钢琴，支持键盘与触屏演奏，适合随手弹奏与试听和弦进行。</p>
        <div class="tags"><span class="tag">Web Audio</span><span class="tag">触屏友好</span></div>
        <div class="go">开始演奏 →</div>
      </a>

      <a class="card proj" href="/go-game/">
        <div class="top">
          <span class="emoji-none">GO</span>
          <div><h3>围棋 · GO</h3><div class="path">/go-game/</div></div>
        </div>
        <p>服务端完成规则、AI 搜索与胜率评估，支持棋局收集、自我对弈与 AI 观战闭环。</p>
        <div class="tags"><span class="tag">PHP 8</span><span class="tag">JIT</span><span class="tag">自学习</span></div>
        <div class="go">进入对局 →</div>
      </a>

      <a class="card proj" href="/turbowarp-mcp/mcp.php">
        <div class="top">
          <span class="emoji-none">MCP</span>
          <div><h3>TurboWarp MCP</h3><div class="path">/turbowarp-mcp/mcp.php</div></div>
        </div>
        <p>通过 MCP Streamable HTTP 让 AI 客户端远程编写 TurboWarp / Scratch 3 项目并导出 <code>.sb3</code>。</p>
        <div class="tags"><span class="tag">JSON-RPC</span><span class="tag">31 个工具</span></div>
        <div class="go">查看文档 →</div>
      </a>

      <a class="card proj" href="https://gitee.com/wssjh822" target="_blank" rel="noopener">
        <div class="top">
          <span class="emoji-none">&lt;/&gt;</span>
          <div><h3>源码仓库</h3><div class="path">GitHub · Gitee</div></div>
        </div>
        <p>所有项目的源代码都可以在 GitHub 与 Gitee 上找到，欢迎提交 issue 与建议。</p>
        <div class="tags"><span class="tag">开源</span><span class="tag">C++</span><span class="tag">PHP</span></div>
        <div class="go">前往仓库 →</div>
      </a>
    </div>
  </section>

  <!-- ===================== 下载 ===================== -->
  <section id="downloads">
    <div class="sec-head">
      <div class="eyebrow">Downloads</div>
      <h2>下载最新版本</h2>
      <p>最新版为 <b>Lux <?= e($latest) ?></b>，历史版本与包管理系统源码可在注册表的下载页获取。</p>
    </div>
    <div class="dl-list">
      <?php
        if (!$luxDl && !$serverDl) {
            echo '<div class="dl-row"><div class="meta"><div class="t">下载清单暂不可用</div>'
               . '<div class="d">请前往包注册表的下载页查看源码与归档。</div></div>'
               . '<a class="btn small" href="/lux/?p=downloads">下载页</a></div>';
        }
        if ($serverDl) {
            $d = $serverDl[0];
      ?>
      <div class="dl-row">
        <span class="badge-v"><?= e((string)($d['version'] ?? $latest)) ?></span>
        <div class="meta">
          <div class="t"><?= e((string)($d['title'] ?? 'Lux 包管理系统源码')) ?></div>
          <div class="d"><?= e((string)($d['desc'] ?? '注册表 API + 网页界面 + 部署说明')) ?></div>
        </div>
        <span class="sz"><?= e(fmt_size($d['size'] ?? 0)) ?></span>
        <a class="btn small" href="/lux/downloads/<?= e(rawurlencode((string)($d['file'] ?? ''))) ?>">下载</a>
      </div>
      <?php } ?>
      <?php foreach (array_slice($luxDl, 0, 5) as $d): ?>
      <div class="dl-row">
        <span class="badge-v"><?= e((string)($d['version'] ?? '')) ?></span>
        <div class="meta">
          <div class="t"><?= e((string)($d['title'] ?? 'Lux 编译器源码')) ?></div>
          <div class="d"><?= e((string)($d['desc'] ?? '完整源码（C++）')) ?></div>
        </div>
        <span class="sz"><?= e(fmt_size($d['size'] ?? 0)) ?></span>
        <a class="btn small" href="/lux/downloads/<?= e(rawurlencode((string)($d['file'] ?? ''))) ?>">下载</a>
      </div>
      <?php endforeach; ?>
      <div class="dl-row">
        <div class="meta">
          <div class="t">全部版本与校验值</div>
          <div class="d">每个归档都附带 SHA-256，可在注册表下载页查看。</div>
        </div>
        <a class="btn small" href="/lux/?p=downloads">查看全部</a>
      </div>
    </div>
  </section>

  <!-- ===================== 关于 ===================== -->
  <section id="about">
    <div class="sec-head">
      <div class="eyebrow">About</div>
      <h2>关于作者</h2>
    </div>
    <div class="about">
      <div class="card">
        <p style="margin-top:0">
          Lux 由一名学生独立设计与实现：从词法、语法、类型系统到两个代码生成后端，
          以及本站的包注册表与各类实验项目，都是长期在课余时间一点一点打磨出来的。
          如果你对语言实现感兴趣，欢迎交流。
        </p>
        <dl class="kv">
          <dt>姓名</dt><dd>沈佳豪</dd>
          <dt>身份</dt><dd>学生</dd>
          <dt>邮箱</dt>
          <dd>
            <a href="mailto:wssjh822@163.com">wssjh822@163.com</a><br>
            <a href="mailto:aswedfrtghyuj@163.com">aswedfrtghyuj@163.com</a><br>
            <a href="mailto:wssjh822@qq.com">wssjh822@qq.com</a>
          </dd>
          <dt>网站</dt><dd><a href="https://lux.xfes.top/">lux.xfes.top</a></dd>
        </dl>
        <div class="links">
          <a class="btn small" href="https://github.com/wssjh822" target="_blank" rel="noopener">GitHub</a>
          <a class="btn small" href="https://gitee.com/wssjh822" target="_blank" rel="noopener">Gitee</a>
          <a class="btn small" href="/lux/?p=feedback">意见反馈</a>
        </div>
      </div>
      <div class="card">
        <h3 style="margin-top:0">快速入口</h3>
        <p>常用页面一览，直接点击即可前往。</p>
        <div style="display:grid;gap:10px;margin-top:16px">
          <a class="btn" href="/lux/">Lux 包注册表</a>
          <a class="btn" href="/lux/?p=browse">浏览全部包</a>
          <a class="btn" href="/lux/?p=downloads">源码下载</a>
          <a class="btn" href="/lux/?p=feedback">意见反馈</a>
          <a class="btn" href="/hex.html">编码转换编辑器</a>
          <a class="btn" href="/piano.html">鎏金钢琴</a>
          <a class="btn" href="/go-game/">围棋 · GO</a>
        </div>
      </div>
    </div>
  </section>

</main>

<footer>
  <div class="wrap inner">
    <div>© <?= e($year) ?> 沈佳豪 · lux.xfes.top</div>
    <div>
      <a href="/lux/">包注册表</a> ·
      <a href="https://github.com/wssjh822" target="_blank" rel="noopener">GitHub</a> ·
      <a href="https://gitee.com/wssjh822" target="_blank" rel="noopener">Gitee</a>
    </div>
  </div>
</footer>

</body>
</html>
