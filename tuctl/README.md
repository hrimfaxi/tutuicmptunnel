[English](./README.md) | [简体中文](./README_zh-CN.md)

---

# tuctl

`tuctl` is the userspace controller for `tutuicmptunnel`. It does not need to stay resident in memory — it exits immediately after configuration; from that point on, the eBPF program loaded in memory provides the service independently.

## Quick Start

### Load BPF and attach to interface `eth0`

```bash
sudo tuctl load iface eth0
```

### Configure the server

```bash
sudo tuctl server                                    # Enter server mode
sudo tuctl server-add uid 42 address 10.0.0.2 port 51820 comment "wg peer"
```

The server will convert ICMP Echo packets from `10.0.0.2` with `ICMP code` 42 into UDP packets destined for port `51820`; for all responses from that UDP address, the server will convert them back into ICMP packets and send them to the original source address.

### Configure the client

```bash
sudo tuctl client                                    # Enter client mode
sudo tuctl client-add address 198.51.100.7 port 51820 uid 42
```

UDP data sent by the client to destination address `198.51.100.7` on destination port `51820` will be encapsulated as ICMP packets; ICMP Reply packets from `198.51.100.7` with `ICMP code` 42 will be converted back to UDP packets by the client.

## Subcommand Reference

All subcommands must be run as `root`. Keywords are case-insensitive and can be abbreviated (e.g., `addr` instead of `address`).

### `load`

> Load the BPF program and attach it to one or more interfaces. This step is required to enable `tutuicmptunnel`.

```text
tuctl load iface IFACE [iface IFACE2 ...] [debug] [ethhdr] [no-ethhdr]
```

| Parameter | Description |
| ---- | ---- |
| `iface IFACE` | Specify network interface; at least one is required |
| `debug` | Load debug build (with logging/tracing) |
| `ethhdr` | Force Ethernet frame parsing (most interface types, e.g. `eth0` or `wlan0`) |
| `no-ethhdr` | Force no Ethernet header (e.g. `pppoe-wan`) |

> [!NOTE]
> If neither `ethhdr` nor `no-ethhdr` is specified, the link type is auto-detected.

### `unload`

> Remove the TC filter from the interface and unload the BPF. It immediately deletes the BPF from the kernel filesystem.

```text
tuctl unload iface IFACE [iface IFACE2 ...]
```

### `server`

> Switch to server role.

```text
tuctl server [max-age SECS] [no-fixup]
```

| Parameter | Default | Description |
| ---- | ------ | ---- |
| `max-age` | `60` | UDP session aging time, in seconds |
| `no-fixup` | None | Disable hardware checksum fixup |

> [!WARNING]
> Do not use `no-fixup` unless necessary.

### `server-add`

> Add a client entry on the server side.

```text
tuctl server-add [OPTIONS] {uid UID | user USERNAME} address ADDR port PORT [icmp-id ID] [sport PORT] [comment COMMENT]
```

| Parameter | Default | Description |
| ---- | ------ | ---- |
| `uid` | None | User UID |
| `user` | None | Username |
| `address` | None | Client source address: `IPv4` / `IPv6` / domain name |
| `port` | None | Server destination port |
| `icmp-id` | `0` | Optional: ICMP ID used by the client |
| `sport` | `0` | Optional: Client UDP source port |
| `comment` | Empty string | Optional: Descriptive comment to identify the client |

Optional parameters:

| Parameter | Description |
| ---- | ---- |
| `-4` | Use only `IPv4` addresses when resolving domain names |
| `-6` | Use only `IPv6` addresses when resolving domain names |
| `-n` | Display UID numbers instead of usernames |

### `server-del`

> Delete a client entry on the server side.

```text
tuctl server-del [OPTIONS] {uid UID | user USERNAME}
```

| Parameter | Default | Description |
| ---- | ------ | ---- |
| `uid` | None | User UID |
| `user` | None | Username |

Optional parameters:

| Parameter | Description |
| ---- | ---- |
| `-n` | Display UID numbers instead of usernames |

### `client`

> Switch to client role.

```text
tuctl client [OPTIONS]
```

Optional parameters:

| Parameter | Description |
| ---- | ---- |
| `no-fixup` | Disable hardware checksum fixup |

> [!WARNING]
> Do not use `no-fixup` unless necessary.

### `client-add`

> Add a remote server entry to the client configuration.

```text
tuctl client-add [OPTIONS] address ADDR port PORT {uid UID | user USERNAME} [comment COMMENT]
```

| Parameter | Default | Description |
| ---- | ------ | ---- |
| `address` | None | Server address: `IPv4` / `IPv6` / domain name |
| `port` | None | Server UDP port |
| `uid` | None | User UID |
| `user` | None | Username |
| `comment` | Empty string | Optional: Descriptive comment to identify the server |

Optional parameters:

| Parameter | Description |
| ---- | ---- |
| `-4` | Use only `IPv4` addresses when resolving domain names |
| `-6` | Use only `IPv6` addresses when resolving domain names |
| `-n` | Display UID numbers instead of usernames |

### `client-del`

> Delete a remote server entry from the client configuration.

```text
tuctl client-del [OPTIONS] {uid UID | user USERNAME} address ADDRESS
```

| Parameter | Default | Description |
| ---- | ------ | ---- |
| `address` | None | Server address: `IPv4` / `IPv6` / domain name |
| `uid` | None | User UID |
| `user` | None | Username |

The client allows the same UID to use different server addresses, so you must also specify the server address to delete when removing a server entry.

### `status`

> View current configuration and status.

```text
tuctl status [OPTIONS] [debug]
```

Parameters:

| Parameter | Description |
| ---- | ---- |
| `debug` | Print more debug information |

Optional parameters:

| Parameter | Description |
| ---- | ---- |
| `-n` | Display UID numbers instead of usernames |

### `reaper`

> Server only: clean up expired NAT sessions.

```text
tuctl reaper
```

> [!NOTE]
> If `bpf_timer` is supported, this command is not needed — `tutuicmptunnel` will automatically clean up expired NAT sessions.

### `script`

> Batch mode: read multiple commands from a file or `-` (standard input).

```text
tuctl script FILE
```

Syntax:

- Lines starting with `#` are comments (quoting/escaping supported)
- Empty lines are ignored

### `dump`

> Output the current `tutuicmptunnel` configuration to stdout; can be redirected to a file and later restored with `tuctl script`.

Optional parameters:

| Parameter | Description |
| ---- | ---- |
| `-n` | Display UID numbers instead of usernames |

### `version`

> Print the version number.

### `help`

> Print help information.

## XOR Obfuscation

The BPF version of `tutuicmptunnel` does **not** support XOR obfuscation. If you need XOR obfuscation, please use the kmod version [tutuicmptunnel-kmod](https://github.com/hrimfaxi/tutuicmptunnel-kmod) with `ktuctl`.

The BPF version's design philosophy is to delegate encryption and integrity verification to upper-layer tools (such as WireGuard, Hysteria, Xray, etc.), while only handling encapsulation and forwarding itself.
