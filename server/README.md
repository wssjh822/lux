# Lux 包注册表服务端（1.1.0）

> 数据文件自 0.9.3 起改用 `.php` 后缀 + PHP 退出头守卫（旧版 `.json` 首次请求时
> 自动迁移后删除）；1.0.0 只把 `LUX_VERSION` 对齐到正式版，未改存储格式。

`lux.php` 是一个无数据库依赖的单文件 PHP 注册表 API，`index.php` 是同目录的
网页界面，`lib.php` 是两者共用的公共库。`luxc` 默认从
`https://lux.xfes.top/lux/lux.php` 拉取索引。

## 功能

- **账号**：网页在线注册 / 登录 / 退出；密码用 `password_hash`（bcrypt）保存，
  纯文件存储（`data/users.json`、`data/tokens.json`）。
- **令牌散列存储（0.9.2）**：`tokens.json` 只保存令牌的 SHA-256；
  即使文件泄露也无法直接冒充发布者。
- **速率限制（0.9.2）**：注册（10 次/小时/IP）、上传（60 次/小时/IP）与
  反馈（5 次/小时/IP）按 `data/ratelimit/` 下的文件计数，超限返回“操作过于频繁”。
- **上传需账号**：网页上传 / 编辑 / 删除，以及 `luxc publish` / `luxc unpublish`
  都要求登录，且**只能操作自己发布的包**（管理员除外）。
- **两种提供方式（0.9.3）**：直接上传打好包的 `.tar.gz`，或上传若干具体文件
  由服务端自动打包（纯 PHP 实现 tar.gz，不依赖 Phar 扩展 / `tar` 命令）。
  整目录上传会保留相对路径，同一个顶层目录会自动去掉。
- **更新包（0.9.3）**：发布时可指定 `base_version`，只提供改动 / 新增的文件，
  服务端在基础版本归档上叠加（同名覆盖、新文件追加）后生成新版本的完整归档。
  压缩包与逐文件两种方式都支持更新包。
- **意见反馈（0.9.3）**：网页 `?p=feedback` 任何人可提交（免登录，按 IP 限流），
  也可走 `lux.php?action=feedback`。提交内容写入 `data/feedback.php`；
  登录用户可查看自己的反馈，管理员可查看全部并标记已处理 / 删除。
- **数据文件加固（0.9.3）**：账号 / 令牌 / 限流 / 反馈等数据文件改用 `.php` 后缀并
  以 `<?php exit; ?>` 开头，旧 `.json` 首次请求自动迁移删除 —— 即使站点没有配置
  拒绝访问 `data/`，直接请求也只会得到空响应。
- **浏览下载**：任何人都能浏览 / 搜索 / 查看 / 下载别人的包。
- **源码下载**：网页「下载」页提供包管理系统源码与 Lux 各版本编译器源码，
  并支持展示 `kind=update` 的更新包条目。

## 目录结构

```
/lux/
  index.php                网页界面（注册 / 登录 / 上传 / 浏览 / 下载 / 反馈）
  lux.php                  注册表 API（luxc 调用）
  lib.php                  公共库：账号 / 令牌 / 包存储 / tar.gz 打包 / 反馈
  data/
    users.php              账号（首次注册者自动成为管理员；带 <?php 守卫）
    tokens.php             API 令牌散列（SHA-256 -> 用户名）
    ratelimit/             按 IP 的注册 / 上传 / 反馈计数（.php）
    feedback.php           意见反馈
  packages/<名字>/
    <版本>.json            元数据（含 owner / sha256 / size）
    <版本>.tar.gz          归档
    latest.json            最高版本元数据的副本
  downloads/               源码下载（由 make_downloads.sh 生成并上传）
    downloads.json         下载清单
    lux-server-<版本>.tar.gz
    lux-<版本>-src.tar.gz
    lux-<旧版本>.zip
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
| `?action=publish` | POST | `meta`(JSON) + `archive`(文件) 或 `files[]`(多文件)；可选 `base_version`，需令牌 |
| `?action=edit` | POST | `name` / `version` + 可改字段，需令牌 |
| `?action=delete` | POST | `name` / 可选 `version`，需令牌 |
| `?action=feedback` | POST | `type` / `content` / `contact`，免登录（限流） |

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

用户提供的元数据会同步写进归档里的 `lux.json`（有则合并、无则生成），
因此即便只上传了散装文件，`luxc install` 也能直接安装。

## 发布包的三种形态

```bash
# 1. 完整包：直接上传压缩包（与旧版一致）
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -F 'meta={"name":"mypkg","version":"0.1.0","main":"lib.lux"}' \
  -F 'archive=@mypkg-0.1.0.tar.gz' \
  https://lux.xfes.top/lux/lux.php?action=publish

# 2. 完整包：上传具体文件，服务端打包（整目录时保留子路径）
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -F 'meta={"name":"mypkg","version":"0.1.0","main":"lib.lux"}' \
  -F 'files[]=@lib.lux' -F 'files[]=@src/util.lux' \
  https://lux.xfes.top/lux/lux.php?action=publish

# 3. 更新包：在 0.1.0 上叠加，只上传改动文件
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -F 'meta={"name":"mypkg","version":"0.2.0","main":"lib.lux"}' \
  -F 'base_version=0.1.0' -F 'files[]=@lib.lux' \
  https://lux.xfes.top/lux/lux.php?action=publish
```

网页端的 `?p=upload` 提供同样的三种形态（发布类型 × 提供方式），
并支持拖拽 / 选择整个文件夹、自动补全 `lux.json`、发布更新包时自动列出基础版本。

## 意见反馈

- 网页：`?p=feedback`，类型（建议 / 问题 / 其他）+ 内容 + 可选联系方式。
- API：`POST lux.php?action=feedback`，字段 `type` / `content` / `contact`。
- 存储：`data/feedback.php`（`id` / `time` / `status=open|done` / `user` / `contact`）。
- 管理员在反馈页可以看到全部反馈并标记已处理 / 重新打开 / 删除。

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
