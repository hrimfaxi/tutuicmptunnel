# xray-core 客户端 + Hysteria 服务端（配合 tutuicmptunnel）

[English](./xray_hysteria.md) | [简体中文](./xray_hysteria_zh-CN.md)

---

## 概述

`xray-core` (v26.2.6 之后) 现已支持 `hysteria` outbound，客户端可以直接使用 `xray-core` 连接 Hysteria 服务端，并继续由 `xray-core` 统一管理出站、路由与分流策略。

本文假定 Hysteria 服务端使用 `3323` 端口，如使用其他端口，请将文中的示例配置一并改为对应端口。

## 服务端配置

服务器端仍然使用 **Hysteria server**，配置方式保持不变。按你现有方式配置：

- 监听地址 / 端口
- TLS 证书
- 认证口令
- 拥塞控制参数
- 带宽与策略限制

请确保：

- 服务端实际监听的 UDP 端口与客户端配置一致
- 防火墙已放行对应的 UDP 端口
- 域名证书与 `serverName` 一致
- 若使用中转、隧道或 `tutuicmptunnel`，对应转发目标端口也同步修改

**必须**为 **Hysteria 服务端** 设置以下环境变量：

```sh
QUIC_GO_DISABLE_GSO=1
```

否则 `tutuicmptunnel` 无法使用。

如果你使用 `systemd` 管理 Hysteria，可以在单元文件中加入：

```ini
[Service]
Environment="QUIC_GO_DISABLE_GSO=1"
```

## xray-core 客户端配置

客户端使用 `xray-core` 的 `hysteria` outbound。示例配置如下：

```json
"outbounds": [
  {
    "tag": "proxy",
    "protocol": "hysteria",
    "settings": {
      "version": 2,
      "address": "your-server.example.com",
      "port": 3323
    },
    "streamSettings": {
      "network": "hysteria",
      "hysteriaSettings": {
        "version": 2,
        "auth": "your_auth_token",
        "congestion": "bbr"
      },
      "security": "tls",
      "tlsSettings": {
        "serverName": "your-server.example.com",
        "alpn": ["h3"]
      }
    }
  }
]
```

**必须**为 **`xray-core` 客户端** 设置以下环境变量：

```sh
QUIC_GO_DISABLE_GSO=1
```

否则 `tutuicmptunnel` 无法使用。

如果你使用 `systemd` 管理 `xray-core`，可以在单元文件中加入：

```ini
[Service]
Environment="QUIC_GO_DISABLE_GSO=1"
```

## 配置要点

- 客户端协议使用 `hysteria`
- `settings.version` 与 `hysteriaSettings.version` 保持为 `2`
- `network` 使用 `"hysteria"`
- `security` 使用 `"tls"`
- `alpn` 使用 `["h3"]`
- 客户端与服务端端口保持一致
- 服务端与客户端都必须设置环境变量 `QUIC_GO_DISABLE_GSO=1`

## tutuicmptunnel 配置

隧道配置与 [hysteria 教程](/docs/hysteria_zh-CN.md) 基本相同，只需将目标端口改为 Hysteria 实际使用的端口（本文示例为 `3323`）。

### 1. 检查 UID

确认服务器与客户端两侧的 `/etc/tutuicmptunnel/uids` 中有共同配置，例如：

```text
123 your_user_name
```

确认两端 BPF 程序已加载，且 `tuctl` 可以正常访问：

```bash
sudo tuctl -d
```

### 2. 配置同步脚本

在客户端保存以下脚本，运行后会同时更新客户端与服务器两侧的隧道规则：

```bash
#!/bin/sh

V() {
  echo "$@"
  "$@"
}

TMP=$(mktemp)
export DEV=enp4s0 # 你的客户端上网接口名

sudo tuctl dump > "$TMP"
sudo tuctl unload iface "$DEV"
sudo tuctl load iface "$DEV"

export TUTU_UID=your_user_name   # 替换为服务器上选好的 uid
export ADDRESS=your-server.example.com # 替换为你的服务端域名或 IP
export PORT=3323                 # Hysteria 服务端 UDP 端口

sudo tuctl script - < "$TMP"
rm -f "$TMP"
sudo tuctl client
sudo tuctl client-del address "$ADDRESS" user "$TUTU_UID"
sudo tuctl client-add address "$ADDRESS" port "$PORT" user "$TUTU_UID"

export COMMENT=your_client_name  # 替换为客户端注释
export HOST="$ADDRESS"
export PSK=your_psk_here         # 替换为你的 tuctl_server PSK
export SERVER_PORT=14801         # 替换为你的 tuctl_server 端口

printf "server\nserver-add uid $TUTU_UID address @client_ip@ port $PORT comment $COMMENT\n" | V tuctl_client \
  psk "$PSK" \
  server "$HOST" \
  server-port "$SERVER_PORT"

# vim: set sw=2 ts=2 et:
```

### 3. 启动

在启动 xray-core 客户端前运行上述脚本即可。如果使用 systemd 管理 xray-core，可以把环境变量和预启动脚本一起写进单元文件，免去手动执行：

```ini
[Service]
Environment="QUIC_GO_DISABLE_GSO=1"
ExecStartPre=/path/to/your/script.sh
```

## 开机自启与 IP 变更

如果客户端公网 IP 会变化，可以使用 `crontab` 或 `systemd` timer 定期调用上述脚本自动更新，具体写法参见 [hysteria 教程](/docs/hysteria_zh-CN.md) 的「定时同步客户端 IP」一节。

