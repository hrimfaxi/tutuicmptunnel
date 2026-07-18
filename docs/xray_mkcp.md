# xray+mKCP

[English](./xray_mkcp.md) | [简体中文](./xray_mkcp_zh-CN.md)

---

## Overview

`xray-core` `mKCP` can replace the `xray-core` + `kcptun` dual-process solution. The main advantages are:

- Eliminates overhead from copying and context switching between dual processes.
- `mKCP` is more friendly to `xray-core`, with more efficient scheduling and implementation.
- Natively supports concurrent multiple `mKCP` connections, avoiding `KCP` head-of-line blocking.

## xray-core Configuration

Recommended Server Configuration:

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
        "alpn": ["h2","http/1.1"],
        "certificates": [
          {
            "certificateFile": "/etc/xray/xray.crt",
            "keyFile": "/etc/xray/xray.key"
          }
        ]
      }
    }
  }
]
```

Recommended Client Configuration:

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
              "id": "your_vps_id",
              "alterId": 0,
              "security": "auto",
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

Key Points:

- Capacity: Set `uplinkCapacity` close to the actual available uplink; set `downlinkCapacity` larger (e.g., 100) to avoid downlink bottlenecks.
- Server `writeBufferSize` is roughly equivalent to the `sndwnd` of original `KCP`; setting it too high may cause a packet flood, overwhelming the client or network queues.
- Practical testing experience: Client `readBufferSize ≈ ceil(server writeBufferSize × 1.5)` balances throughput and latency better.
- Others: Fine-tune `mtu`, `tti`, and `congestion` based on the link and hardware, and observe throughput, `RTT`, packet loss, and `CPU` usage.
- It is recommended to test the `tti` value in actual practice, as this part is somewhat counter-intuitive: sending too fast may not necessarily improve performance, but might trigger `ICMP` rate limiting, making it difficult to improve efficiency.

## tutuicmptunnel Configuration

First, check if there is a common `/etc/tutuicmptunnel/uids` configuration on both server/client sides:

```
123 your_user_name
```

Then check if BPF programs are loaded on both ends and `tuctl` can access the device.

```sh
sudo tuctl -d
```

Run the following simple script on the client to sync the configuration to both server/client sides:

```sh
#!/bin/sh

V() {
  echo "$@"
  "$@"
}

TMP=$(mktemp)
export DEV=enp4s0 # Your client's internet interface name

sudo tuctl dump > $TMP
sudo tuctl unload iface $DEV
sudo tuctl load iface $DEV

export TUTU_UID=your_user_name # Replace with the uid chosen on your server
export ADDRESS=yourdomain.com # Replace with your xray-core server domain or IP
export PORT=1234 # Replace with your xray-core server udp port

sudo tuctl script - < $TMP
sudo tuctl load iface "$DEV"
rm -f $TMP
sudo tuctl client
sudo tuctl client-del address $ADDRESS user $TUTU_UID
sudo tuctl client-add address $ADDRESS port $PORT user $TUTU_UID

export COMMENT=your_client_name # Replace with your client comment; this will appear in the server's tuctl command output
export HOST=$ADDRESS
export PSK=yourlongpsk # Replace with your tuctl_server PSK password
export SERVER_PORT=14801 # Replace with your tuctl_server port

printf "server\nserver-add uid $TUTU_UID address @client_ip@ port $PORT comment $COMMENT\n" | V tuctl_client \
psk $PSK \
server $HOST \
server-port $SERVER_PORT

# vim: set sw=2 ts=2 expandtab:
```

Just run the script before starting the `xray-core` client. You can also add `ExecStartPre` to the `xray-core` `systemd` unit file to run this script automatically.
This way, you don't have to run the script every time.

## Enable on Boot

See [hysteria](hysteria.md). You can invoke the above script periodically using `crontab` or `systemd-timer`.

## See Also

- [xray-core mkcp documentation](https://xtls.github.io/en/config/transports/mkcp.html)
