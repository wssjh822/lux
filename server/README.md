# Lux 包注册表服务端（0.9.2）

`lux.php` 是一个无数据库依赖的单文件 PHP 注册表 API，`index.php` 是同目录的
网页界面。`luxc` 默认从 `https://lux.xfes.top/lux/lux.php` 拉取索引。

## 功能

- **账号**：网页在线注册 / 登录 / 退出；密码用 `password_hash`（bcrypt）保存，
  纯文件存储（`data/users.json`、`data/tokens.json`）。
- **令牌散列存储（0.9.2）**：`tokens.json` 只保存令牌的 SHA-256；
  即使文件泄露也无法直接冒充发布者。
- **速率限制（0.9.2）**：注册（10 次/小时/IP）与上传（60 次/小时/IP）
  按 `data/ratelimit/` 下的文件计数，超限返回“操作过于频繁”。
- **上传需账号**：网页上传 / 编辑 / 删除，以及 `luxc publish` / `luxc unpublish`
  都要求登录，且**只能操作自己发布的包**（管理员除外）。
- **浏览下载**：任何人都能浏览 / 搜索 / 查看 / 下载别人的包。
- **源码下载**：网页「下载」页提供包管理系统源码与 Lux 各版本编译器源码。

## 目录结构

```
/lux/
  index.php                网页界面（注册 / 登录 / 上传 / 浏览 / 下载）
  lux.php                  注册表 API（luxc 调用）
  lib.php                  公共库：账号 / 令牌 / 包存储 / 所有权校验
  data/
    users.json             账号（首次注册者自动成为管理员）
    tokens.json            API 令牌散列（SHA-256 -> 用户名）
    ratelimit/             按 IP 的注册 / 上传计数
  packages/<名字>/
    <版本>.json            元数据（含 owner / sha256 / size）
    <版本>.tar.gz          归档
    latest.json            最高版本元数据的副本
  downloads/               源码下载（由 make_downloads.sh 生成并上传）
    downloads.json         下载清单
    lux-server-<版本>.tar.gz
    lux-<版本>.zip         当前版本源码（0.9.4 起用 zip，顶层目录为 lux/）
    lux-<旧版本>.zip       历史版本源码
```

> `data/`、上传的包、`downloads/` 都不要提交到版本库（已在 `.gitignore`）。

## API

| 动作 | 方法 | 说明 |
| --- | --- | --- |
| `?action=health` | GET | 健康检查 |
| `?action=index` | GET | 全部包的全部版本 |
| `?action=search&q=` | GET | 搜索，只返回最新版本 |
| `?action=info&name=` | GET | 单包全部版本 |
| `?action=download&name=&version=` | GET | 下载归档 |
| `?action=register` | POST | `username` / `password` / `email` → `{token,user}` |
| `?action=login` | POST | `username` / `password` → `{token,user}` |
| `?action=logout` | POST | 令牌失效 |
| `?action=whoami` | POST | 当前用户 |
| `?action=mine` | POST | 我发布的包 |
| `?action=publish` | POST | `meta`(JSON) + `archive`(文件)，需令牌 |
| `?action=edit` | POST | `name` / `version` + 可改字段，需令牌 |
| `?action=delete` | POST | `name` / 可选 `version`，需令牌 |

令牌通过 `Authorization: Bearer <token>` 请求头（或表单 `token` 字段）传递。

元数据字段与 `luxc` 的 `parseIndex`（`src/pkgs.cpp`）一一对应：

```json
{
  "name": "numx", "version": "0.1.0",
  "summary": "…", "description": "…",
  "main": "lib.lux", "files": ["lib.lux"],
  "deps": { "mathx": "^0.1.0" },
  "tags": ["math"], "authors": ["…"],
  "license": "MIT", "homepage": "…", "lux": ">=0.8.0",
  "owner": "alice",
  "url": "https://…/packages/numx/0.1.0.tar.gz",
  "sha256": "…", "size": 1234
}
```

## 部署

1. 把 `lux.php`、`lib.php`、`index.php` 传到站点某目录（如 `/lux/`）。
2. 保证该目录可写（服务端会自动建 `data/`、`packages/`）。
3. 删除旧的 `index.html`（改用 `index.php`）。
4. 用 `server/make_downloads.sh` 生成下载包并上传到 `downloads/`。

> **部署顺序（重要）**：先把服务部署到**内网 / 限制访问**的状态，
> **自己先注册一个账号**（第一个注册者自动成为管理员），确认能登录、
> 能发布 / 删除后，**再开放公网**。否则第一个访问者会抢到管理员。
> 若已经被抢，可在服务端手工编辑 `data/users.json` 把 `admin` 改到自己的键上。

或一键部署（需要 FTP 凭据）：

```bash
LUX_FTP_HOST=lux.example.com LUX_FTP_USER=... LUX_FTP_PASS=... \
LUX_FTP_ROOT=/lux server/deploy.sh
```

## 命令行发布

```bash
luxc login alice             # 保存令牌到 ~/.lux/config.json
luxc whoami
luxc publish ./mypkg         # 打包 + 上传（HTTP 账号 API）
luxc unpublish mypkg 0.1.0   # 删除某个版本
luxc logout
```

## 本地调试

```bash
php -S 127.0.0.1:8098 -t server
LUX_REGISTRY=http://127.0.0.1:8098/lux.php luxc update
LUX_REGISTRY=http://127.0.0.1:8098/lux.php luxc install numx
# 网页： http://127.0.0.1:8098/index.php
```

## 换用第三方注册表

只要外部地址返回相同的 JSON 结构即可：

```bash
luxc registry https://my-mirror.example/lux.php
# 或临时
LUX_REGISTRY=https://my-mirror.example/lux.php luxc install foo
```
