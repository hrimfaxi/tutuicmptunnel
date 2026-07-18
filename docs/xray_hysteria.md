# xray + hysteria

[English](./xray_hysteria.md) | [简体中文](./xray_hysteria_zh-CN.md)

---

## Overview

`xray-core` (since v26.2.6) supports `hysteria` outbound. This allows the client to connect to a Hysteria server directly through `xray-core`, while continuing to use `xray-core` for outbound management, routing, and traffic splitting.

This article assumes that the Hysteria server uses port `3323`. If you use a different port, replace the example port in the configurations below accordingly.

## Server configuration

The server side still uses **Hysteria server**, and the configuration method remains unchanged. Configure it as you normally would:

- Listen address / port
- TLS certificate
- Authentication token
- Congestion control parameters
- Bandwidth and policy limits

Make sure that:

- The actual UDP port listened on by the server matches the client configuration
- The firewall allows the corresponding UDP port
- The domain certificate matches `serverName`
- If relays, tunnels, or `tutuicmptunnel` are used, the forwarding target port is updated accordingly as well

You **must** set the following environment variable for the **Hysteria server**:

```sh
QUIC_GO_DISABLE_GSO=1
```

Otherwise, `tutuicmptunnel` will not work.

If you manage Hysteria with `systemd`, add the following to the service unit:

```ini
[Service]
Environment="QUIC_GO_DISABLE_GSO=1"
```

## xray-core client configuration

The client uses `xray-core` with a `hysteria` outbound. Example configuration:

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

You **must** set the following environment variable for the **`xray-core` client**:

```sh
QUIC_GO_DISABLE_GSO=1
```

Otherwise, `tutuicmptunnel` will not work.

If you manage `xray-core` with `systemd`, add the following to the service unit:

```ini
[Service]
Environment="QUIC_GO_DISABLE_GSO=1"
```

## Configuration notes

- Use `hysteria` as the client protocol
- Keep both `settings.version` and `hysteriaSettings.version` set to `2`
- Use `"hysteria"` for `network`
- Use `"tls"` for `security`
- Use `["h3"]` for `alpn`
- The client and server ports must match
- Both server and client must set `QUIC_GO_DISABLE_GSO=1`

## `tutuicmptunnel` configuration

The tunnel configuration is mostly the same as the [hysteria tutorial](/docs/hysteria.md), just change the target port to the actual Hysteria port (this example uses `3323`).

### 1. Check UID

Check whether both server and client have a shared `/etc/tutuicmptunnel/uids` configuration:

```txt
123 your_user_name
```

Verify that BPF programs are loaded on both ends and `tuctl` can access the device:

```sh
sudo tuctl -d
```

### 2. Configuration sync script

Run the following script on the client. It will sync the configuration to both the server and the client:

```sh
#!/bin/sh

V() {
  echo "$@"
  "$@"
}

TMP=$(mktemp)
export DEV=enp4s0 # your client uplink interface name

sudo tuctl dump > "$TMP"
sudo tuctl unload iface "$DEV"
sudo tuctl load iface "$DEV"

export TUTU_UID=your_user_name   # replace with the UID selected on the server
export ADDRESS=your-server.example.com # replace with your server domain or IP
export PORT=3323                 # Hysteria server UDP port

sudo tuctl script - < "$TMP"
rm -f "$TMP"
sudo tuctl client
sudo tuctl client-del address "$ADDRESS" user "$TUTU_UID"
sudo tuctl client-add address "$ADDRESS" port "$PORT" user "$TUTU_UID"

export COMMENT=your_client_name  # replace with a comment for this client
export HOST="$ADDRESS"
export PSK=your_psk_here         # replace with your tuctl_server PSK
export SERVER_PORT=14801         # replace with your tuctl_server port

printf "server\nserver-add uid $TUTU_UID address @client_ip@ port $PORT comment $COMMENT\n" | V tuctl_client \
  psk "$PSK" \
  server "$HOST" \
  server-port "$SERVER_PORT"

# vim: set sw=2 ts=2 et:
```

### 3. Start

Run the script before starting the `xray-core` client. If you use `systemd` to manage `xray-core`, you can add both the environment variable and the pre-start script to the unit file:

```ini
[Service]
Environment="QUIC_GO_DISABLE_GSO=1"
ExecStartPre=/path/to/your/script.sh
```

## Start at boot & IP changes

If the client's public IP changes, you can use `crontab` or `systemd-timer` to periodically run the script above. See [hysteria tutorial](/docs/hysteria.md) "Periodic client IP sync" section for details.
