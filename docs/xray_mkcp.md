# Xray mKCP + tutuicmptunnel

[English](./xray_mkcp.md) | [简体中文](./xray_mkcp_zh-CN.md)

---

## Overview

`xray-core`'s built-in `mKCP` transport can replace the "`xray-core` + `kcptun`" dual-process approach. Main advantages:

- Eliminates data copying and context switching overhead between two processes;
- `mKCP` has tighter integration with `xray-core`'s scheduling and implementation, more efficient;
- Natively supports multiple `mKCP` connections concurrently, avoiding head-of-line blocking of single `KCP` connection.

After layering `tutuicmptunnel`, intermediate nodes still only see ICMP packets. The overall link is as follows:

```mermaid
flowchart LR
    subgraph C["Client"]
        xray_c["xray-core<br>outbound: mKCP/UDP"]
        tmt_c["tutuicmptunnel<br>UDP → ICMP Encapsulation"]
        ctl_c["tuctl_client<br>(Called by sync script)"]
        xray_c -->|"mKCP/UDP, Destination Port 1234"| tmt_c
    end

    subgraph S["Server"]
        tmt_s["tutuicmptunnel<br>ICMP → UDP Decapsulation"]
        xray_s["xray-core<br>inbound :1234/UDP (mKCP)"]
        ctl_s["tuctl_server<br>Listen :14801"]
        tmt_s --> xray_s
    end

    tmt_c == "ICMP (Public, Intermediate Nodes Only See This)" ==> tmt_s
    ctl_c -. "UDP 14801 Control Channel, Register uid" .-> ctl_s
```

## Prerequisites

- Both ends have `xray-core` installed;
- Server has TLS certificates ready (example paths: `/etc/xray/xray.crt` and `/etc/xray/xray.key`);
- Both ends have `tutuicmptunnel` and supporting tools (`tuctl`, `tuctl_client`, `tuctl_server`) installed.

Ports involved:

| Port | Protocol | Location | Purpose |
| ----- | ---- | ------ | ----------------------------- |
| 1234 | UDP | Server | `xray-core` mKCP inbound |
| 14801 | UDP | Server | `tuctl_server` listening port |

## Configure xray-core

### Server Configuration

```json
"inbounds": [
  {
    "protocol": "vless",
    "port": 1234,
    "settings": {
      "decryption": "none",
      "clients": [
        {
          "id": "your_xray_id",
          "flow": "xtls-rprx-vision"
        }
      ]
    },
    "streamSettings": {
      "network": "kcp",
      "kcpSettings": {
        "mtu": 1450,
        "tti": 100,
        "congestion": true,
        "uplinkCapacity": 5,
        "downlinkCapacity": 100,
        "readBufferSize": 2,
        "writeBufferSize": 3
      },
      "security": "tls",
      "tlsSettings": {
        "minVersion": "1.3",
        "alpn": ["h2", "http/1.1"],
        "certificates": [
          {
            "certificateFile": "/etc/xray/xray.crt",
            "keyFile": "/etc/xray/xkey.key"
          }
        ]
      }
    }
  }
]
```

### Client Configuration

```json
"outbounds": [
  {
    "tag": "proxy",
    "protocol": "vless",
    "settings": {
      "vnext": [
        {
          "address": "your_vps_ip",
          "port": 1234,
          "users": [
            {
              "id": "your_xray_id",
              "encryption": "none",
              "flow": "xtls-rprx-vision"
            }
          ]
        }
      ]
    },
    "streamSettings": {
      "network": "kcp",
      "kcpSettings": {
        "mtu": 1450,
        "tti": 100,
        "congestion": true,
        "uplinkCapacity": 2,
        "downlinkCapacity": 100,
        "readBufferSize": 5,   // ≈ ceil(server writeBufferSize(3) × 1.5)
        "writeBufferSize": 1
      },
      "security": "tls",
      "tlsSettings": {
        "allowInsecure": false,
        "serverName": "your_vps_domain_name",
        "alpn": ["h2"],
        "fingerprint": "chrome"
      }
    },
    "mux": { "enabled": false, "concurrency": -1 }
  }
]
```

> Note: The user `id` must be the same on both ends (i.e. `your_xray_id` is the same placeholder).

Key points:

- **Capacity**: `uplinkCapacity` / `downlinkCapacity` are in MB/s, used for mKCP rate estimation, not rate limiting. Set `uplinkCapacity` close to actual available uplink; set `downlinkCapacity` larger (e.g. 100) to avoid downlink bottlenecks.
- **Buffer**: `readBufferSize` / `writeBufferSize` are in MB. Server's `writeBufferSize` is roughly equivalent to `sndwnd` in the kcptun approach — setting it too high may cause a packet flood, overwhelming the client or network queues.
- **Empirical values**: Client `readBufferSize` ≈ $\lceil \textit{writeBufferSize}_{\text{server}} \times 1.5 \rceil$ (e.g. server=3, client=5) balances throughput and latency well.
- **tti**: In ms, and somewhat counter-intuitive — shorter send intervals don't necessarily improve performance; after `tutuicmptunnel` encapsulation, excessively high packet rates may trigger ICMP rate limiting on the link. Test different values in practice.
- **Other**: Fine-tune `mtu`, `congestion` based on the link and hardware, and observe throughput, RTT, packet loss, and CPU usage.

## Configure tutuicmptunnel

First, confirm that both ends have a consistent uid entry in `/etc/tutuicmptunnel/uids` (format: `uid username`):

```text
123 your_user_name
```

Then verify that BPF programs are loaded on both ends and `tuctl` can access the device:

```bash
sudo tuctl -d
```

Run the following script on the client to simultaneously deploy configuration to both client and server:

```bash
#!/bin/sh

V() {
  echo "$@"
  "$@"
}

TMP=$(mktemp)
DEV=enp4s0                # Client's uplink interface name

sudo tuctl dump > "$TMP"
sudo tuctl unload iface "$DEV"
sudo tuctl load iface "$DEV"

TUTU_UID=your_user_name   # UID assigned on server (matches /etc/tutuicmptunnel/uids)
ADDRESS=yourdomain.com    # xray-core server domain or IP
PORT=1234                 # Server xray-core mKCP inbound UDP port

sudo tuctl script - < "$TMP"
rm -f "$TMP"
sudo tuctl client
sudo tuctl client-del address "$ADDRESS" user "$TUTU_UID"   # Delete old entry first, idempotent
sudo tuctl client-add address "$ADDRESS" port "$PORT" user "$TUTU_UID"

COMMENT=your_client_name  # Client comment, visible in server's tuctl output
HOST=$ADDRESS
PSK=yourlongpsk           # tuctl_server PSK
SERVER_PORT=14801         # tuctl_server listening port

printf "server\nserver-add uid $TUTU_UID address @client_ip@ port $PORT comment $COMMENT\n" | V tuctl_client \
  psk "$PSK" \
  server "$HOST" \
  server-port "$SERVER_PORT"

# vim: set sw=2 ts=2 expandtab:
```

Run the script before starting the `xray-core` client. You can also add it to the `xray-core` systemd unit as an `ExecStartPre` to run automatically:

```ini
[Service]
ExecStartPre=/usr/local/bin/tutuicmptunnel_sync.sh
```

Don't forget to make it executable: `sudo chmod +x /usr/local/bin/tutuicmptunnel_sync.sh`.

## Enable on Boot

See [hysteria](hysteria.md) for how to use `crontab` or a systemd timer to periodically call the script above for auto-start.

## See Also

- [xray-core mKCP documentation](https://xtls.github.io/en/config/transports/mkcp.html)
