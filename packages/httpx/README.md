# httpx — Lux 极简 HTTP 客户端

**无 TLS，仅 HTTP。** URL 必须以 `http://` 开头；`https://` 直接返回 `none`。
统一发送 `Connection: close` 并读到 EOF，不做 chunked 解码 / keep-alive /
gzip / 重定向（这些是 1.2 明确不做的范围，见 `docs/net.md`）。

```bash
luxc install httpx
```

```lux
import "httpx";

fn main() {
    let r = get!("http://example.com/");
    println(r.status);
}
```

## API（v1，冻结）

| 函数 | 说明 |
| --- | --- |
| `get(url) -> HttpResponse?` | GET 请求 |
| `post(url, body, content_type) -> HttpResponse?` | POST 请求 |
| `header(resp, name) -> string?` | 大小写不敏感取响应头 |
| `parse_response(raw) -> HttpResponse?` | 纯字符串响应解析（零网络，可单测） |
| `parse_url(url) -> string[]?` | URL 解析，返回 `[host, port, path]` |

`struct HttpResponse { status: int; reason: string; header_keys: string[]; header_vals: string[]; body: string; }`

> `parse_response` / `parse_url` 是解析辅助函数，方便在不联网的情况下测试。
