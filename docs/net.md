# Lux 1.2 设计：`net` 标准库与 `httpx` 包

> 状态：**1.2.0 已落地**。本文拍死 1.2 的全部边界，实现按本文执行；
> 格式沿用 `fn.md` / `arc.md`。新语法零、ARC 零侵入、双后端必达、
> 外网零依赖的测试 —— 四条红线。

## 1. 范围与冻结 API

1.2 只做 **TCP 客户端 / 服务端的 7 个原语**（进 `stdlib`，双后端）+
> `httpx` 纯 Lux 包。**TLS/HTTPS 明确不做**（顺延 1.3 拍板），写进
> `stability.md` 的「不包含清单」。

```lux
import "net";
```

| 函数 | 签名 | 说明 |
| --- | --- | --- |
| `dial` | `dial(host: string, port: int) -> int?` | 含 DNS-lite + `connect`，成功给 socket fd |
| `send` | `send(fd: int, data: string) -> int?` | 发送字节数；失败 `none` + `last_error()` |
| `recv` | `recv(fd: int, max: int) -> string?` | 最多 `max` 字节；`""` = EOF；失败 `none` |
| `close` | `close(fd: int) -> bool` | 关闭 fd |
| `listen` | `listen(port: int) -> int?` | `bind 0.0.0.0` + `listen`，监听 fd |
| `accept` | `accept(lfd: int) -> int?` | 等待并返回连接 fd |
| `set_timeout` | `set_timeout(fd: int, secs: float) -> bool` | 设置 `SO_RCVTIMEO` / `SO_SNDTIMEO` |

原语级函数（`socket` / `bind` / `connect` 裸调用 / UDP 收发对）**不暴露**。

`last_error()` 新增模板（随 1.2 冻结，两后端逐字一致）：

```
net.dial   失败：无法解析主机 '<host>'   / 连接被拒绝 / 网络不可达
net.send   失败：连接已断开
net.recv   失败：超时 （其余失败：连接已断开）
net.listen 失败：端口已被占用 （其余失败：无法监听端口）
net.accept 失败：无法接受连接
```

## 2. DNS-lite 算法（双实现，行为对齐）

只做 IPv4，两个后端各自实现一遍同一算法，靠回环差分对齐：

1. 若是点分十进制字面量 → 直接解析（不做任何网络访问）。
2. 否则查 `/etc/hosts`（逐行取第一个字段为 IP、其余为别名，取第一个匹配）。
3. 否则读 `/etc/resolv.conf`，取**第一个** `nameserver`。
4. 向该 nameserver 的 53/udp 发一条 A 记录查询，跟随应答里的 CNAME
   （**上限 8 跳**），取应答里**第一个 A 记录**。

不做：AAAA / IPv6、EDNS、TCP 回退、search domain、DNS 缓存。

## 3. HTTP 语义（`httpx` v1）

`httpx` 统一发 `Connection: close`，**读到 EOF 为止** —— 这样绕开
chunked 解码与 keep-alive 状态机。不做 gzip、不做重定向。

响应解析是**纯字符串函数** `parse_response(raw) -> HttpResponse?`，
可用构造输入做零网络单元测试（见 `tests/pkgs/httpx_test.lux`）。

## 4. 连接超时

1.2 只提供 `net.set_timeout`（映射 `SO_RCVTIMEO` / `SO_SNDTIMEO`）；
`dial` 的 `connect` 阶段用**内核默认超时**，不额外设。文档如实写明。
测试全部走 `127.0.0.1`，拒绝 / 超时是即时的，不受影响。

## 5. 明确不做（1.2 的不包含清单）

TLS/HTTPS、IPv6/AAAA、`connect` 阶段超时、chunked 解码、keep-alive /
连接池、gzip、重定向、异步 / epoll、UDP 收发对用户暴露、
`net` 原语级函数外露、闭包、`map<K,V>`。

## 6. 表示与内存

- **socket fd 是裸 int，完全不碰 ARC。** 不是堆对象，不进 `pending`，
  不需要 `retain` / `release`。忘了 `close` 就是 fd 泄漏，仅此而已。
- `recv` 的内部缓冲上限 **1 MiB**（对齐 `lx_read_line` 惯例）。
- 原生后端把 `sockaddr_in` 用 16 字节数据缓冲 + `__poke16` / `__poke32`
  拼出来（family@0、port@2 大端、addr@4 大端、zero@8）。

## 7. 双后端实现对照

| 层 | C 后端 | 原生后端 |
| --- | --- | --- |
| 内建枚举 | `Builtin::Net*` | 同左 |
| 传输 | `libc` socket 家族（`lx_net_*`） | 特权内建 `__sys_socket` / `__sys_connect` / `__sys_sendto` / `__sys_recvfrom` / `__sys_bind` / `__sys_listen` / `__sys_accept` / `__sys_setsockopt` + `__poke16` / `__poke32` |
| 架构差异 | 无 | `accept` 在 aarch64 走 `accept4(..., flags=0)`；其余 syscall 号由发射器按架构给出 |
| DNS-lite | C 版（`/etc/hosts` → UDP） | Lux 版（`bytes` / `__peek8u` 解析 UDP 应答） |
| 运行时 | 内联进生成的 C | `native_rt.lux` 的 `luxrt_net_*` |

行为对齐靠 `tests/run_tests.sh` 的「网络差分」一节：C/Native ×
client/server 的对角矩阵 + `set_timeout` 超时文案逐字节一致。

## 8. 测试矩阵

- `tests/net/`：`server.lux` / `client_echo.lux` / `server_silent.lux` /
  `client_timeout.lux`，双后端各编一份，回环对拍。
- `tests/cases/`：`net_dial_refused`、`net_hosts_resolve`、`net_dns_lite`。
- `tests/errors/`：`net_bad_args`、`net_unknown_fn`。
- `tests/pkgs/httpx_test.lux`：纯字符串响应解析（零网络）。
- 全程超时保护，禁外网。

## 9. 落地顺序

`net.md` 拍板 → `sema` / `lux.hpp` / `loader`（编译期）→ kRuntime C 版 →
原生 `__sys_*` + `__poke16/32` + `luxrt` Lux 版 → 回环差分跑通 →
`httpx` 包 → 文档全量对齐 → 版本号链 → 发布。
