# tutuicmptunnel + MOSH（固定 UDP 端口 3325）

本文演示如何把 **MOSH 的 UDP 会话端口固定为一个端口**（例如 `3325/udp`），并用 **tutuicmptunnel** 将该 UDP 流量封装进 **ICMP echo request/reply**，从而在 UDP 被封锁/不稳定的网络环境中仍可使用 mosh。

## 背景原理

1. **MOSH 不是纯 SSH**
   mosh 启动时会先走 **SSH**（用于认证、启动服务端进程），随后真正的交互数据走 **UDP**。

2. **服务端必须有 `mosh-server` 进程**
   mosh 客户端会通过 SSH 在远端执行 `mosh-server new ...`，启动一个 UDP 会话端点。

3. **固定端口是为了让隧道好封装**
   mosh 默认会在一个端口范围中选择端口（常见 60000–61000）。
   对 ICMP 隧道来说，固定为单端口或小范围会更稳定、规则更简单。

---

## 服务器（test.server）

1) 安装 mosh：
```bash
# Debian/Ubuntu
sudo apt update && sudo apt install -y mosh
```

2) 放行UDP 3325（如有防火墙）

```bash
sudo ufw allow 3325/udp
```

---

## 客户端

保存为 `mosh-over-icmp.sh`：

```bash
#!/usr/bin/env bash
set -euo pipefail

ADDRESS="test.server"
PORT="3325"
TUTU_UID="199"
SERVER_PORT="14801"
COMMENT="a320-mosh"
PSK="yourlongpsk"

# 按照你的服务器设置
#export TUTUICMPTUNNEL_PWHASH_MEMLIMIT="1024768"

MOSH_USER="${MOSH_USER:-$USER}"
SSH_PORT="${SSH_PORT:-22}"

sudo tuctl client
sudo tuctl client-del uid "$TUTU_UID" address "$ADDRESS"
sudo tuctl client-add uid "$TUTU_UID" address "$ADDRESS" port "$PORT"
sudo tuctl status

IP="$(curl -sf ip.3322.net | tr -d '\r')"
tuctl_client psk "$PSK" server "$ADDRESS" server-port "$SERVER_PORT" \
  <<< "server-add uid $TUTU_UID address $IP port $PORT comment $COMMENT"

if [[ "$SSH_PORT" == "22" ]]; then
  exec mosh --server="mosh-server new -p ${PORT}:${PORT}" \
    "${MOSH_USER}@${ADDRESS}"
else
  exec mosh --ssh="ssh -p ${SSH_PORT}" \
    --server="mosh-server new -p ${PORT}:${PORT}" \
    "${MOSH_USER}@${ADDRESS}"
fi
```

运行：
```bash
chmod +x mosh-over-icmp.sh
./mosh-over-icmp.sh
```

指定用户名/SSH 端口：
```bash
MOSH_USER=root SSH_PORT=2222 ./mosh-over-icmp.sh
```

---

## 验证（可选）

服务端看监听：

```bash
sudo ss -lunp | grep ':3325'
```

客户端抓 ICMP：

```bash
sudo tcpdump -ni any icmp
```
