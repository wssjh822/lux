# lux.xfes.top 首页（站点根目录）

`index.php` 是 <https://lux.xfes.top/> 首页的源码（单文件 PHP，无外部依赖）。

- 版本号、下载列表、包数量都在运行时从 `/lux/downloads/downloads.json` 与
  `/lux/packages/` 动态读取；读取失败时回退到文件内的默认值（`$latest`）。
- 部署：把 `index.php` 上传到站点根目录（FTP 根）即可，无需数据库。
- 修改后建议 `php -l index.php` 校验语法，再上传覆盖。

> 注意：仓库里的 `index.html` 是早期的静态落地页（另一套金色主题），
> 当前线上使用的是本目录的 `index.php`。
