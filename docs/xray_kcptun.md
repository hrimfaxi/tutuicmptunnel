# xray+kcptun

## 概述

`xray-core`是一款常用的翻墙工具。为了提升性能和抗干扰能力，可以结合`kcptun`使用：
首先，将`xray-core`的`inbound`输入（通常为`TCP`端口）转发到本地的`kcptun-server`所监听的`UDP` 端口。
在客户端，需要启动一个`kcptun-client`，负责将本地的 `TCP`（如 `TLS` 流量）转为 `KCP` 协议。
此时，中间节点看到的是`KCP`流量(`UDP`)。有时会被`ISP` `QOS`。
在此基础上，我们可以用`tutuicmptunnel`对`UDP`流量再做一层封装，将其转换为`ICMP`流量。
这样，最终中间节点所看到的，仅仅是`ICMP`报文，进一步提升了流量的隐蔽性和穿透能力。

## 前提

本文假设你已经拥有一套可用的`xray-core`服务端和客户端配置，服务器监听的端口为`20000`（`TCP` `TLS`端口）。
同时，你已经在服务器和客户端两端分别安装好了最新版的 `kcptun-server` 和 `kcptun-client`，放在`/usr/local/bin`目录。
接下来，我们将首先利用`kcptun`将通信路径上的流量从`TCP`转换 为`UDP`，然后再通过`tutuicmptunnel`进一步将`UDP`流量封装为`ICMP`，实现最终的数据穿透和伪装。

### KCPTun-server

创建环境变量文件 `/etc/default/kcptun-server`

```conf
# SMUX版本，一般用2即可
KCPTUN_SMUXVER=2
# 建议按照下面方法随机生成一个
KCPTUN_KEY=LC2N0lx5_Kq6l.6l
# xray的inbound端口
KCPTUN_TARGET=127.0.0.1:20000
# UDP端口监听端口
KCPTUN_LISTEN=:3322
KCPTUN_MODE=fast
KCPTUN_CRYPT=xor
KCPTUN_SOCKBUF=16777217
# kcp发送窗口值
KCPTUN_SNDWND=4096
# kcp接受窗口值
KCPTUN_RCVWND=512
KCPTUN_DATASHARD=0
KCPTUN_PARITYSHARD=0
# kcp mtu值，最高1444
KCPTUN_MTU=1400
```

可以使用`kcptun`的`xor`模式来稍微掩盖一下流量特征。使用以下`python3`命令生成16字节的`key`:

```python
python3 -c "import random, string; print('KCPTUN_KEY=' + ''.join(random.choices(string.ascii_letters + string.digits + '._', k=16)))"
```

服务器和客户端上这个`KCPTUN_KEY`必须一致。

复制下列模板到 `/etc/systemd/system/kcptun@.service`

```ini
[Unit]
Description=KCPTun Server
After=network.target

[Service]
Type=simple
EnvironmentFile=/etc/default/kcptun-%i
DynamicUser=yes
AmbientCapabilities=CAP_NET_BIND_SERVICE
ProtectSystem=full
ProtectHome=yes
NoNewPrivileges=true
PrivateTmp=yes
RestrictSUIDSGID=yes
LimitNOFILE=1048576

ExecStart=/usr/local/bin/kcptun_server \
  --smuxver "$KCPTUN_SMUXVER" \
  -t "$KCPTUN_TARGET" -l "$KCPTUN_LISTEN" \
  -mode "$KCPTUN_MODE" -nocomp \
  --datashard "$KCPTUN_DATASHARD" \
  --parityshard "$KCPTUN_PARITYSHARD" \
  --crypt "$KCPTUN_CRYPT" --key "$KCPTUN_KEY" \
  -sockbuf "$KCPTUN_SOCKBUF" \
  --sndwnd "$KCPTUN_SNDWND" --rcvwnd "$KCPTUN_RCVWND" \
  --mtu "$KCPTUN_MTU"

Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
```

启动并设为开机自启

```bash
sudo systemctl enable --now kcptun@server
```

### KCPTun-client

`/etc/default/kcptun-client-yourhostname`:

```bash
#!/bin/sh

KCPTUN_SMUXVER=2
# 你的服务器地址
KCPTUN_HOST=yourdomain.com
# 你的KCPTUN udp端口号
KCPTUN_PORT=3322
# kcptun-client的本地监听端口
KCPTUN_LOCAL_PORT=3323
KCPTUN_MODE=fast
KCPTUN_NOCOMP=-nocomp
KCPTUN_AUTOEXPIRE=-900
KCPTUN_DATASHARD=0
KCPTUN_PARITYSHARD=0
KCPTUN_CRYPT=xor
# 和服务器一样的xor key
KCPTUN_KEY=LC2N0lx5_Kq6l.6l
KCPTUN_RCVWND=4096
KCPTUN_SNDWND=256
KCPTUN_SOCKBUF=16777217
KCPTUN_MTU=1400
```

编辑以下`systemd`单元文件
`/etc/systemd/system/kcptun-client@.service`:

```ini
[Unit]
Description=KCPTun Client
After=network.target

[Service]
Type=simple

# 载入环境变量（密钥等），systemd 方式
EnvironmentFile=/etc/default/kcptun-client-%i

# 使用 DynamicUser 提高隔离性
DynamicUser=yes

# 程序必须能监听高端口，否则需加 CAP_NET_BIND_SERVICE
AmbientCapabilities=CAP_NET_BIND_SERVICE

# 文件系统和家目录保护
ProtectSystem=full
ProtectHome=yes

# 额外安全限制
NoNewPrivileges=true
PrivateTmp=yes
ProtectHostname=yes
ProtectClock=yes
ProtectKernelModules=yes
ProtectKernelTunables=yes
ProtectControlGroups=yes
RestrictSUIDSGID=yes
RestrictRealtime=yes
RestrictNamespaces=yes
LockPersonality=yes

# 资源限制，可根据实际需求调整
LimitNOFILE=1048576

# 启动命令（变量用环境变量方式传入）
ExecStart=/usr/local/bin/kcptun-client \
    --smuxver "${KCPTUN_SMUXVER}" \
    -r "${KCPTUN_HOST}:${KCPTUN_PORT}" \
    -l ":${KCPTUN_LOCAL_PORT}" \
    -mode "${KCPTUN_MODE}" \
    ${KCPTUN_NOCOMP} \
    -autoexpire "${KCPTUN_AUTOEXPIRE}" \
    --datashard "${KCPTUN_DATASHARD}" \
    --parityshard "${KCPTUN_PARITYSHARD}" \
    --crypt "${KCPTUN_CRYPT}" \
    --key "${KCPTUN_KEY}" \
    --rcvwnd "${KCPTUN_RCVWND}" \
    --sndwnd "${KCPTUN_SNDWND}" \
    -sockbuf "${KCPTUN_SOCKBUF}" \
    --mtu "${KCPTUN_MTU}"

Restart=on-failure
RestartSec=2s

[Install]
WantedBy=multi-user.target
```

启用服务：

```bash
sudo systemctl enable --now kcptun-client@yourhostname
# 检查进程参数是否和你预期一致
ps -ef|grep kcptun
```

### Xray 出口修改

找到你的客户端配置，复制为`xxx-kcp.json`，然后将 `outbound` 指向 `127.0.0.1:3323`（即 `KCPTun` 本地端口），其它配置不变。

运行`xray-core`：

```bash
xray -c xxx-kcp.json
# 使用tcpdump观察是否流量变成了预期的udp流量
tcpdump -i any udp and port 3323 -n -v
```

### 设置tutuicmptunnel

首先服务器/客户端上检查是否有`tutu_csum_fixup`，一般推荐使用它。

```bash
sudo lsmod|grep tutu_csum_fixup
```

使用以下脚本开启`ICMP`：

`/usr/local/bin/tutuicmptunnel_sync.sh`:

```
#!/bin/bash

V() {
  echo "$@"
  "$@"
}

TMP=$(mktemp)
export DEV=enp4s0 # 你的客户端的上网接口名

sudo tuctl dump > $TMP
sudo tuctl unload iface $DEV
sudo tuctl load iface $DEV #ethhdr #debug

export TUTU_UID=yourdevice # 替换为你的服务器上选好的uid
export ADDRESS=yourdomain.com # 替换为你的xray-core服务器域名或IP
export PORT=3323 # 替换为你的xray-core服务器udp端口

sudo tuctl script - < $TMP
sudo tuctl client
sudo tuctl client-add address $ADDRESS port $PORT user $TUTU_UID

export COMMENT=yourdevice # 替换为你的客户端的注释，此注释会在服务器的tuctl命令上显示
export HOST=$ADDRESS
export PSK=yourlongpsk # 替换为你的tuctl_server的PSK口令
export SERVER_PORT=3321 # 替换为你的tuctl_server的端口

# 使用3322.net的ip api服务器获取本机公网IP，你也可以换成其他服务
IP=$(curl -s ip.3322.net)
echo local ip: $IP

V tuctl_client psk $PSK server $HOST server-port $SERVER_PORT <<<"server-add uid $TUTU_UID address $IP port $PORT comment $COMMENT"

# vim: set sw=2 ts=2 expandtab:
```

运行脚本，如果一切正常，你的`kcptun`的`udp`流量将被转换为`icmp`流量。
使用以下命令来观察：

```bash
sudo tcpdump -i any -n icmp -v
```

### 开机自启

参见[hysteria](hysteria.md)，可以定期使用`crontab`或者`systemd-timer`调用`/usr/local/bin/tutuicmptunnel_sync.sh`.

