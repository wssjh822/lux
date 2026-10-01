<?php
// =============================================================================
//  lux.php : Lux 包注册表 API（0.9.2）
//
//  公开动作（GET）：
//    ?action=health                  健康检查
//    ?action=index                   全部包的全部版本
//    ?action=search&q=关键词          过滤，只返回每个包的最高版本
//    ?action=info&name=mathx         单包全部版本
//    ?action=download&name=&version= 下载归档
//  账号动作（POST）：
//    ?action=register  username/password/email
//    ?action=login     username/password        -> {token,user}
//    ?action=logout    token
//    ?action=whoami    token                    -> {user}
//  需登录（POST，带 token）：
//    ?action=publish   meta=<JSON> + archive=<tar.gz 文件>
//    ?action=edit      token/name/version + 可改字段
//    ?action=delete    token/name[/version]
//    ?action=mine      token                    -> 我发布的包
//
//  上传 / 修改 / 删除都要求注册账号，且只能操作自己发布的包。
// =============================================================================
declare(strict_types=1);

require __DIR__ . '/lib.php';
lux_ensure_dirs();

header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Headers: Authorization, Content-Type');
header('Cache-Control: no-cache');
header('X-Content-Type-Options: nosniff');

function respond(array $payload, int $code = 200): void {
    http_response_code($code);
    header('Content-Type: application/json; charset=utf-8');
    echo json_encode($payload, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT);
    exit;
}
function fail(string $msg, int $code = 400): void { respond(['error' => $msg], $code); }
function require_user(): array {
    $u = request_user();
    if (!$u) fail('需要登录（请先 luxc login 或在网页登录）', 401);
    return $u;
}

$action = $_GET['action'] ?? 'index';
$method = $_SERVER['REQUEST_METHOD'] ?? 'GET';

switch ($action) {

case 'health': {
    $all = pkg_list_all();
    respond([
        'ok'       => true,
        'registry' => 'lux.official',
        'version'  => LUX_VERSION,
        'schema'   => LUX_SCHEMA,
        'packages' => count(pkg_latest_only($all)),
        'versions' => count($all),
        'users'    => count(users_load()),
        'time'     => gmdate('c'),
    ]);
    break;
}

case 'index': {
    respond(['schema' => LUX_SCHEMA, 'registry' => 'lux.official',
             'packages' => pkg_list_all()]);
    break;
}

case 'search': {
    $q = trim((string)($_GET['q'] ?? ''));
    $all = pkg_list_all();
    if ($q !== '') {
        $lq = function_exists('mb_strtolower') ? mb_strtolower($q, 'UTF-8') : strtolower($q);
        $all = array_values(array_filter($all, function ($p) use ($lq) {
            $hay = ($p['name'] ?? '') . ' ' . ($p['summary'] ?? '') . ' '
                 . ($p['description'] ?? '') . ' ' . implode(' ', $p['tags'] ?? []);
            $hay = function_exists('mb_strtolower') ? mb_strtolower($hay, 'UTF-8') : strtolower($hay);
            return function_exists('mb_strpos') ? mb_strpos($hay, $lq) !== false
                                                : strpos($hay, $lq) !== false;
        }));
    }
    respond(['schema' => LUX_SCHEMA, 'registry' => 'lux.official',
             'packages' => pkg_latest_only($all)]);
    break;
}

case 'info': {
    $name = (string)($_GET['name'] ?? '');
    if (!valid_component($name)) fail('非法的包名');
    $mine = pkg_versions($name);
    if (!$mine) fail("找不到包 '$name'", 404);
    respond(['schema' => LUX_SCHEMA, 'name' => $name, 'versions' => $mine]);
    break;
}

case 'download': {
    $name = (string)($_GET['name'] ?? '');
    $version = (string)($_GET['version'] ?? '');
    if (!valid_component($name) || !valid_version($version)) fail('非法的包名或版本号');
    $file = pkg_archive_file($name, $version);
    if (!is_file($file)) fail('包文件不存在', 404);
    header('Content-Type: application/gzip');
    header('Content-Length: ' . filesize($file));
    header('Content-Disposition: attachment; filename="' . $name . '-' . $version . '.tar.gz"');
    readfile($file);
    exit;
}

case 'register': {
    if ($method !== 'POST') fail('register 需要 POST', 405);
    $name = trim((string)($_POST['username'] ?? ''));
    $pass = (string)($_POST['password'] ?? '');
    $mail = trim((string)($_POST['email'] ?? ''));
    $err = '';
    $u = user_register($name, $pass, $mail, $err);
    if (!$u) fail($err);
    $token = token_issue($u['name']);
    respond(['ok' => true, 'user' => ['name' => $u['name'], 'email' => $u['email']],
             'token' => $token]);
    break;
}

case 'login': {
    if ($method !== 'POST') fail('login 需要 POST', 405);
    $name = trim((string)($_POST['username'] ?? ''));
    $pass = (string)($_POST['password'] ?? '');
    $u = user_verify($name, $pass);
    if (!$u) fail('用户名或密码错误', 401);
    $token = token_issue($u['name']);
    respond(['ok' => true, 'user' => ['name' => $u['name'], 'email' => $u['email'] ?? ''],
             'token' => $token]);
    break;
}

case 'logout': {
    token_revoke((string)request_token());
    respond(['ok' => true]);
    break;
}

case 'whoami': {
    $u = require_user();
    respond(['ok' => true, 'user' => ['name' => $u['name'], 'email' => $u['email'] ?? '',
                                      'admin' => !empty($u['admin'])]]);
    break;
}

case 'mine': {
    $u = require_user();
    $mine = array_values(array_filter(pkg_list_all(),
        fn($p) => ($p['owner'] ?? '') === $u['name']));
    respond(['ok' => true, 'user' => $u['name'], 'packages' => $mine]);
    break;
}

case 'publish': {
    if ($method !== 'POST') fail('publish 需要 POST', 405);
    $u = require_user();
    $metaRaw = (string)($_POST['meta'] ?? '');
    $meta = json_decode($metaRaw, true);
    if (!is_array($meta)) fail('缺少或无法解析 meta（应为 JSON）');
    $err = '';
    $saved = pkg_store_upload($_FILES['archive'] ?? [], $meta, $u['name'],
                              !empty($u['admin']), $err);
    if (!$saved) fail($err);
    respond(['ok' => true, 'name' => $saved['name'], 'version' => $saved['version'],
             'url' => $saved['url'], 'sha256' => $saved['sha256'], 'size' => $saved['size']]);
    break;
}

case 'edit': {
    if ($method !== 'POST') fail('edit 需要 POST', 405);
    $u = require_user();
    $name = (string)($_POST['name'] ?? '');
    $version = (string)($_POST['version'] ?? '');
    if (!valid_component($name) || !valid_version($version)) fail('非法的包名或版本号');
    if (!is_file(pkg_meta_file($name, $version))) fail('该版本不存在', 404);
    if (!pkg_owned_by($name, $version, $u['name']) && empty($u['admin'])) {
        fail('只能修改自己发布的包', 403);
    }
    $meta = json_decode((string)@file_get_contents(pkg_meta_file($name, $version)), true);
    if (!is_array($meta)) fail('元数据损坏', 500);

    $strFields = ['summary', 'description', 'main', 'license', 'homepage', 'lux'];
    foreach ($strFields as $f) {
        if (array_key_exists($f, $_POST)) $meta[$f] = trim((string)$_POST[$f]);
    }
    // 数组字段：逗号或换行分隔
    foreach (['files', 'tags', 'authors'] as $f) {
        if (!array_key_exists($f, $_POST)) continue;
        $parts = preg_split('/[\r\n,]+/', (string)$_POST[$f]) ?: [];
        $meta[$f] = array_values(array_filter(array_map('trim', $parts), fn($x) => $x !== ''));
    }
    // 依赖：每行 name@约束 或 name
    if (array_key_exists('deps', $_POST)) {
        $deps = [];
        foreach (preg_split('/[\r\n]+/', (string)$_POST['deps']) ?: [] as $line) {
            $line = trim($line);
            if ($line === '') continue;
            $at = strrpos($line, '@');
            if ($at !== false && $at > 0) $deps[substr($line, 0, $at)] = trim(substr($line, $at + 1));
            else $deps[$line] = '*';
        }
        $meta['deps'] = $deps;
    }
    // 允许改版本说明（不移动文件，只改 tag 字段可选）
    $meta['updated_at'] = gmdate('c');
    if (!pkg_write_meta($name, $version, $meta)) fail('写入元数据失败', 500);
    respond(['ok' => true, 'name' => $name, 'version' => $version]);
    break;
}

case 'delete': {
    if ($method !== 'POST') fail('delete 需要 POST', 405);
    $u = require_user();
    $name = (string)($_POST['name'] ?? '');
    $version = trim((string)($_POST['version'] ?? ''));
    if (!valid_component($name)) fail('非法的包名');
    $vers = pkg_versions($name);
    if (!$vers) fail('包不存在', 404);
    if ($version !== '') {
        if (!valid_version($version)) fail('非法的版本号');
        $found = false;
        foreach ($vers as $v) if ($v['version'] === $version) $found = true;
        if (!$found) fail('该版本不存在', 404);
        if (!pkg_owned_by($name, $version, $u['name']) && empty($u['admin'])) {
            fail('只能删除自己发布的包', 403);
        }
        pkg_remove_version($name, $version);
        respond(['ok' => true, 'name' => $name, 'version' => $version]);
    }
    // 删除整包：所有版本都必须属于自己
    foreach ($vers as $v) {
        if (($v['owner'] ?? '') !== $u['name'] && empty($u['admin'])) {
            fail('包 ' . $name . ' 里有不是你的版本，不能整包删除', 403);
        }
    }
    pkg_remove_all($name);
    respond(['ok' => true, 'name' => $name]);
    break;
}

default:
    fail("未知 action '$action'", 400);
}
