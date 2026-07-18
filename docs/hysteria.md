# hysteria

[English](./hysteria.md) | [简体中文](./hysteria_zh-CN.md)

---

## Example Demonstration

> Local -(SOCKS5)→ Hysteria-UDP -(ICMP)→ Server \
> All parameters have been abstracted as placeholders. Please replace them according to your actual situation, especially domains/keys. \
> ⚠️ The documentation intentionally omits real sensitive information like SNI, passwords, IPs, etc. Do not leave plaintext credentials in public spaces.

We assume you currently have a working `hysteria` configuration with `UDP` port `3322`.
Now you need to use `tutuicmptunnel` to transform the traffic into the `ICMP` protocol.

## Dependencies

`Ubuntu`:

```bash
sudo apt-get install curl
```

`Archlinux`:

```bash
sudo pacman -S curl
```

## `tutuicmptunnel` Settings

First, you need to select a `UID` for your client device on the server. For example, let's say the hostname is `a320` and the `UID` is 100.

`/etc/tutuicmptunnel/uids`:

```
100 a320 # a320
```

Add the above record to `/etc/tutuicmptunnel/uids` on the client side as well.

## Modify Systemd Unit File

Note that `tutuicmptunnel` cannot handle `GSO` packets, so `hysteria`'s related functionality needs to be disabled.
Set the environment variable `QUIC_GO_DISABLE_GSO=1` as shown below:

`/etc/systemd/system/hysteria-server@.service`:

```ini
[Unit]
Description=Hysteria Server (%i.yaml)
After=network.target tutuicmptunnel.service

[Service]
Type=simple
ExecStart=/usr/local/bin/hysteria server --config /etc/hysteria/%i.yaml
DynamicUser=yes
Environment=HYSTERIA_LOG_LEVEL=info QUIC_GO_DISABLE_GSO=1
CapabilityBoundingSet=CAP_NET_ADMIN CAP_NET_BIND_SERVICE CAP_NET_RAW
AmbientCapabilities=CAP_NET_ADMIN CAP_NET_BIND_SERVICE CAP_NET_RAW
NoNewPrivileges=true
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

## Clean Up Expired Sessions

Session cleanup is only needed if the architecture does not support `bpf_timer` (`mips` or older `ubuntu` versions).

```bash
sudo cp contrib/etc/systemd/system/tutuicmptunnel-server-reaper.timer /etc/systemd/system/
sudo systemctl enable --now tutuicmptunnel-server-reaper.timer
```

### Client IP Sync Script

You can use the `tuctl_client` tool on the client to remotely update the server's `tuctl` configuration. This way, when the client's public IP changes, the server can be notified.

`/usr/local/bin/tutuicmptunnel_sync.sh`:

```bash
#!/bin/bash

# This script runs on the client, notifying the server of client config updates via tuctl_client.

V() {
  echo "$@"
  "$@"
}

TMP=$(mktemp)
export DEV=eth0 # Your client's internet interface name
sudo tuctl dump > $TMP
sudo tuctl unload iface $DEV
sudo tuctl load iface $DEV

export TUTU_UID=100 # Replace with the uid chosen on your server
export ADDRESS=yourdomain.com # Replace with your hysteria server domain or IP
export PORT=3322 # Replace with your hysteria server udp port

sudo tuctl script - < $TMP
sudo tuctl client
sudo tuctl client-del address $ADDRESS user $TUTU_UID
sudo tuctl client-add address $ADDRESS port $PORT user $TUTU_UID

export COMMENT=yourdevice # Replace with a comment for your client; this will appear in the server's tuctl command output
export HOST=$ADDRESS
export PSK=yourlongpsk # Replace with your tuctl_server PSK password
export SERVER_PORT=your_tuserver_port # Replace with your tuctl_server port

V tuctl_client psk $PSK server $HOST server-port $SERVER_PORT <<<"server-add uid $TUTU_UID address @client_ip@ port $PORT comment $COMMENT"

# vim: set sw=2 ts=2 expandtab:
```

### Start `tutuicmptunnel`

```bash

# First sync the client IP to the server
/usr/local/bin/tutuicmptunnel_sync.sh

# Then run the hysteria client
QUIC_GO_DISABLE_GSO=1 hysteria client -c client.yaml
```

If the client's public IP changes frequently, you need to update the client IP to the server. You can add the above script to `crontab` to run periodically (every 5 minutes):

```
PATH=/usr/local/sbin:/usr/local/bin:/usr/bin:/usr/sbin:/bin:/sbin
*/5 * * * * /usr/local/bin/tutuicmptunnel_sync.sh
```

Or use a `systemd-timer` to achieve the same goal:

`/etc/systemd/system/tutuicmptunnel_sync.service`:

```ini
[Unit]
Description=Sync tutuicmptunnel config

[Service]
Type=oneshot
Environment=PATH=/usr/local/sbin:/usr/local/bin:/usr/bin:/usr/sbin:/bin:/sbin
ExecStart=/usr/local/bin/tutuicmptunnel_sync.sh
```

`/etc/systemd/system/tutuicmptunnel_sync.timer`:

```
[Unit]
Description=Run tutuicmptunnel_sync every 5 minutes

[Timer]
OnBootSec=5min
OnUnitActiveSec=5min
Persistent=true

[Install]
WantedBy=timers.target
```

## Testing

Speed test:

```bash
hysteria speedtest -c client.yaml 
```

View ICMP tunnel counters:

```bash
sudo tuctl -d
```

BPF logs:

```bash
sudo bpftool prog tracelog
```

Check for ICMP packets:

```bash
sudo tcpdump -i any icmp -n -v
```
