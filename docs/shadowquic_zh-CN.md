# shadowquic

`shadowquic` 是一个基于 **Rust** 与 **quinn(QUIC)** 的代理工具，整体思路与 Hysteria 类似：使用 **UDP/QUIC** 传输，以获得更高吞吐与更好的弱网表现。

> 版本要求：请使用 **0.3.3 或更新版本**，以便支持关闭 **GSO**（用于配合 `tutuicmptunnel`）。

---

## 特点与优势

1. **速度快，资源占用更低**
   在我的测试环境（mipsle，小米 R3G）中，`shadowquic` 相比 `hysteria` 内存占用更低（约 1/3）；高负载时 CPU 占用约 50%。

2. **使用 JLS（免证书部署）**
   `shadowquic` 使用 JLS：一种 TLS 1.3 风格的握手/伪装与认证方案，主要依赖双方共享凭据，而非传统证书体系。因此部署时通常不需要像 `hysteria` 那样依赖 ACME 申请证书或自建证书链。

   - 客户端配置中的 `server-name` 用于握手外观/指纹对齐
   - 一般需要与服务端的 `jls_upstream` 保持一致

3. **Rust 的内存安全**
   Rust 能显著降低传统 C/C++ 中常见的内存安全风险。

---

## 安装与配置

### 服务器端

安装并启动服务：

```bash
curl -L https://raw.githubusercontent.com/spongebob888/shadowquic/main/scripts/linux_install.sh | bash
systemctl daemon-reload
systemctl enable --now shadowquic.service
```

#### 为配合 tutuicmptunnel：关闭 GSO

编辑 `/etc/shadowquic/server.yaml`，在 `inbound` 下添加（或确认存在）：

```yaml
inbound:
  ...
  gso: false
```

然后重启服务：

```sh
systemctl restart shadowquic.service
```

---

### 客户端

将服务器脚本生成的用户名/密码填入 `client.yaml`，并同样**关闭 GSO**：

```yaml
inbound:
  type: socks
  bind-addr: "127.0.0.1:20808"

outbound:
  type: shadowquic
  addr: "1.2.3.4:1443"
  username: "username"           # 用户名
  password: "password"           # 密码
  server-name: "cloudflare.com"  # 通常需与服务端 jls_upstream 一致，用于外观/指纹对齐
  alpn: ["h3"]
  initial-mtu: 1000
  congestion-control: bbr
  zero-rtt: true
  gso: false                     # for tutuicmptunnel
  over-stream: false             # true: UDP over stream；false: UDP over datagram

log-level: "info"
```

启动客户端：

```bash
shadowquic -c client.yaml
```

此时即可使用 `tutuicmptunnel` 进行保护，方法与 [hysteria](/docs/hysteria_zh-CN.md) 类似，端口改为1443即可。
