# xray+kcptun

[English](./xray_kcptun.md) | [简体中文](./xray_kcptun_zh-CN.md)

---

## Overview

`xray-core` is a widely used proxy tool. To improve performance and anti-interference capabilities, it can be combined with `kcptun`.
First, forward the `xray-core` `inbound` input (usually a `TCP` port) to the `UDP` port listened to by the local `kcptun-server`.
On the client side, a `kcptun-client` needs to be started, responsible for converting local `TCP` (such as `TLS` traffic) into the `KCP` protocol.
At this point, intermediate nodes see `KCP` traffic (`UDP`). This is sometimes subject to `ISP` `QOS`.
On top of this, we can use `tutuicmptunnel` to further encapsulate the `UDP` traffic, converting it into `ICMP` traffic.
In this way, what intermediate nodes ultimately see are merely `ICMP` packets, further enhancing traffic stealthiness and penetration capabilities.

## Prerequisites

This article assumes you already have a working `xray-core` server and client configuration, with the server listening on port `20000` (`TCP` `TLS` port).
Simultaneously, you have installed the latest versions of `kcptun-server` and `kcptun-client` on both the server and client, placed in the `/usr/local/bin` directory.
Next, we will first use `kcptun` to convert traffic on the communication path from `TCP` to `UDP`,
and then use `tutuicmptunnel` to further encapsulate the `UDP` traffic into `ICMP` to achieve final data penetration and camouflage.

### KCPTun-server

Create the environment variable file `/etc/default/kcptun-server`:

```conf
# SMUX version, usually 2
KCPTUN_SMUXVER=2
# Recommended to generate a random one using the method below
KCPTUN_KEY=LC2N0lx5_Kq6l.6l
# xray inbound port
KCPTUN_TARGET=127.0.0.1:20000
# UDP listening port
KCPTUN_LISTEN=:3322
KCPTUN_MODE=fast
KCPTUN_CRYPT=xor
KCPTUN_SOCKBUF=16777217
# kcp send window size
KCPTUN_SNDWND=4096
# kcp receive window size
KCPTUN_RCVWND=512
KCPTUN_DATASHARD=0
KCPTUN_PARITYSHARD=0
# kcp mtu value, max 1444
KCPTUN_MTU=1400
```

You can use `kcptun`'s `xor` mode to slightly mask traffic characteristics. Use the following `python3` command to generate a 16-byte `key`:

```python
python3 -c "import random, string; print('KCPTUN_KEY=' + ''.join(random.choices(string.ascii_letters + string.digits + '._', k=16)))"
```

This `KCPTUN_KEY` must be consistent between the server and the client.

Copy the following template to `/etc/systemd/system/kcptun@.service`:

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

Start and enable on boot:

```bash
sudo systemctl enable --now kcptun@server
```

### KCPTun-client

`/etc/default/kcptun-client-yourhostname`:

```bash
#!/bin/sh

KCPTUN_SMUXVER=2
# Your server address
KCPTUN_HOST=yourdomain.com
# Your KCPTUN udp port number
KCPTUN_PORT=3322
# kcptun-client local listening port
KCPTUN_LOCAL_PORT=3323
KCPTUN_MODE=fast
KCPTUN_NOCOMP=-nocomp
KCPTUN_AUTOEXPIRE=-900
KCPTUN_DATASHARD=0
KCPTUN_PARITYSHARD=0
KCPTUN_CRYPT=xor
# Same xor key as server
KCPTUN_KEY=LC2N0lx5_Kq6l.6l
KCPTUN_RCVWND=4096
KCPTUN_SNDWND=256
KCPTUN_SOCKBUF=16777217
KCPTUN_MTU=1400
```

Edit the following `systemd` unit file `/etc/systemd/system/kcptun-client@.service`:

```ini
[Unit]
Description=KCPTun Client
After=network.target

[Service]
Type=simple

# Load environment variables (keys, etc.), systemd style
EnvironmentFile=/etc/default/kcptun-client-%i

# Use DynamicUser to improve isolation
DynamicUser=yes

# Program must be able to listen on high ports, otherwise add CAP_NET_BIND_SERVICE
AmbientCapabilities=CAP_NET_BIND_SERVICE

# File system and home directory protection
ProtectSystem=full
ProtectHome=yes

# Extra security restrictions
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

# Resource limits, adjust according to actual needs
LimitNOFILE=1048576

# Start command (variables passed via environment variables)
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

Enable the service:

```bash
sudo systemctl enable --now kcptun-client@yourhostname
# Check if process parameters match your expectations
ps -ef|grep kcptun
```

### Xray Outbound Modification

Find your client configuration, copy it as `xxx-kcp.json`, then point `outbound` to `127.0.0.1:3323` (i.e., `KCPTun` local port), leaving other configurations unchanged.

Run `xray-core`:

```bash
xray -c xxx-kcp.json
# Use tcpdump to observe if traffic has become the expected udp traffic
tcpdump -i any udp and port 3323 -n -v
```

### Setting up tutuicmptunnel

Use the following script to enable `ICMP`:

`/usr/local/bin/tutuicmptunnel_sync.sh`:

```
#!/bin/bash

V() {
  echo "$@"
  "$@"
}

TMP=$(mktemp)
export DEV=enp4s0 # Your client's internet interface name

sudo tuctl dump > $TMP
sudo tuctl unload iface $DEV
sudo tuctl load iface $DEV

export TUTU_UID=yourdevice # Replace with the uid chosen on your server
export ADDRESS=yourdomain.com # Replace with your xray-core server domain or IP
export PORT=3323 # Replace with your xray-core server udp port

sudo tuctl script - < $TMP
sudo tuctl client
sudo tuctl client-add address $ADDRESS port $PORT user $TUTU_UID

export COMMENT=yourdevice # Replace with a comment for your client; this will appear in the server's tuctl command output
export HOST=$ADDRESS
export PSK=yourlongpsk # Replace with your tuctl_server PSK password
export SERVER_PORT=3321 # Replace with your tuctl_server port

echo "server-add uid $TUTU_UID address @client_ip@ port $PORT comment $COMMENT" | V tuctl_client \
  psk $PSK \
  server $HOST \
  server-port $SERVER_PORT

# vim: set sw=2 ts=2 expandtab:
```

Run the script. If everything is normal, your `kcptun` `udp` traffic will be converted to `icmp` traffic.
Use the following command to observe:

```bash
sudo tcpdump -i any -n icmp -v
```

### Enable on Boot

Refer to [hysteria](hysteria.md). You can periodically invoke `/usr/local/bin/tutuicmptunnel_sync.sh` using `crontab` or `systemd-timer`.
