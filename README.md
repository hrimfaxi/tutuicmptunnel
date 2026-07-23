[English](./README.md) | [简体中文](./README_zh-CN.md)

---

<div align="center">

# 🚇 tutuicmptunnel

**A `UDP` ⇄ `ICMP` tunneling tool based on `eBPF`**

Can serve as a high-performance alternative to `udp2raw` ICMP mode.
Recommended to be used in combination with [`shadowquic`](docs/shadowquic.md) (officially recommended best companion) / `kcptun` / `hysteria` / `wireguard` and other tools,
jointly coping with increasingly strict UDP QoS and packet loss policies, effectively improving penetration capability and connection stability.

> **🎯 Official Recommendation**: [shadowquic](docs/shadowquic.md) is the **officially recommended best companion** for `tutuicmptunnel` — a Rust-based QUIC proxy optimized for unstable and high packet-loss networks, perfectly complementing ICMP tunneling.

[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL%20v2%2B-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20OpenWrt-orange.svg)](#-operating-system-requirements-and-dependencies)
[![Language](https://img.shields.io/badge/language-C%20%2B%20BPF-brightgreen.svg)](#)
[![Benchmark](https://img.shields.io/badge/benchmark-blue.svg)](docs/benchmark.md)

[Features](#-features) • [Architecture](#-architecture-overview) • [Installation](#-installation) • [Usage](#-quick-start) • [Use Cases](#-main-use-cases) • [Acknowledgements](#-acknowledgements)

</div>

---

## ✨ Features

- 🚀 **High Performance** — Under the same CPU, maximum throughput is several times faster than `udp2raw`, with much lower CPU usage (see [Benchmark](docs/benchmark.md))
- 🔐 **Secure Design** — Configuration synchronization uses `XChaCha20-Poly1305` + `Argon2id` + timestamp + PSK authentication
- 🌐 **Dual-Stack Support** — Simultaneously supports `ICMP` / `ICMPv6` over IPv4 / IPv6
- 🔄 **Hot Configuration Sync** — Securely and quickly synchronize server and client configuration via `tuctl_server` / `tuctl_client`
- 🔗 **Interoperable** — Interchangeable communication with the kmod version [tutuicmptunnel-kmod](https://github.com/hrimfaxi/tutuicmptunnel-kmod) (one end as server, the other as client)
- 🧩 **Single Responsibility** — Handles encapsulation, forwarding, and simple XOR obfuscation; encryption and integrity verification are delegated to upper-layer tools (WireGuard / hysteria / xray, etc.)

---

## 🏗 Architecture Overview

The core of `tutuicmptunnel` is an **eBPF program** loaded onto the network interface, handling UDP ⇄ ICMP packet transformation in kernel space. Each host can only play one of the two roles: server or client.

The data path is as follows:

```mermaid
flowchart LR
    subgraph Client["🖥️ Client"]
        APP["Upper-layer application<br/>(kcptun / hysteria / wireguard ...)"]
        BPF_C["eBPF program<br/>UDP → ICMP encapsulation"]
        APP -- "UDP packets" --> BPF_C
    end

    subgraph Internet["🌍 Intermediate Network"]
        GW["GFW / ISP / Gateway<br/>can only see ICMP Echo / Reply"]
    end

    subgraph Server["☁️ Server"]
        BPF_S["eBPF program<br/>ICMP → UDP restoration"]
        UPSTREAM["Target service<br/>(kcptun-server / wg ...)"]
        BPF_S -- "Restored UDP packets" --> UPSTREAM
    end

    BPF_C -- "ICMP Echo" --> GW
    GW -- "ICMP Echo" --> BPF_S
    UPSTREAM -. "UDP response" .-> BPF_S
    BPF_S -. "ICMP Reply" .-> GW
    GW -. "ICMP Reply" .-> BPF_C
```

Configuration synchronization path (optional, independent of the data plane):

```mermaid
sequenceDiagram
    autonumber
    participant C as tuctl_client (Client)
    participant S as tuctl_server (Server)

    Note over C,S: UDP communication · XChaCha20-Poly1305 encryption<br/>Argon2id key derivation · Timestamp anti-replay · PSK authentication
    C->>S: Report new configuration (uid / @client_ip@ / port / comment)
    S->>S: Update forwarding triple [UID, Client IP, Port]
    S-->>C: Acknowledgement
    Note over C,S: Real-time hot update when client IP changes, no manual SSH login required
```

### 🆔 UID Mechanism

- To distinguish packets from different clients, each client connecting to the server is assigned a unique `UID` (range **0 ~ 255**), which maps to the `code` field of the `ICMP` protocol — therefore **each server supports a maximum of 256 clients**.
- `UID` is only unique within the scope of each server: a client can use the same `UID` on different servers, or use different `UIDs` to connect to multiple servers.
  - For example: map host `1.2.3.4:3322` to `uid 100`, and host `2.3.4.5:2233` to `uid 101`.

### 📦 Forwarding Triple

| Role | Triple | Purpose |
| :--- | :--- | :--- |
| Client | `[UID, Server IP, Target Port]` | Identifies which `UDP` packets need to be converted to `ICMP` packets |
| Server | `[UID, Client IP, Target Port]` | Identifies which `ICMP` packets need to be restored and forwarded as `UDP` packets |

> 💡 IP addresses can be IPv4 or IPv6; `tutuicmptunnel` will automatically select `ICMP` or `ICMPv6` for encapsulation and forwarding based on the IP type.

### 🎯 Design Principles

- Can be used in combination with `WireGuard`, `xray-core` + `kcptun`, `hysteria`, `shadowquic` and other tools. Since these tools already have built-in encryption and integrity verification capabilities, `tutuicmptunnel` provides simple XOR obfuscation and is **not responsible for encryption and verification**, mainly handling encapsulation and forwarding.
- **Does not modify packet payload content**, nor does it add extra IP headers to packets.
- Server-side forwarding rules are entirely configured manually by the user via triples (can be invoked via [tuctl](tuctl/README.md) over SSH, or dynamically synchronized using `tuctl_client`).

---

## 🔍 Survey Preparation: Step 0

Before deploying `tutuicmptunnel`, first verify whether the current link is suitable for carrying an ICMP tunnel. The core idea: **compare the actual performance of ICMP vs UDP on the same link**.

```mermaid
flowchart TD
    A["① Comparative test<br/>ping -f (ICMP flood)<br/>vs<br/>iperf3 -u (UDP)"] --> B{ICMP performance<br/>significantly better than UDP?}
    B -- "✅ Yes" --> C["Link is worth deploying<br/>Continue installation 🎉"]
    B -- "❌ No" --> D["Investigate intermediate devices:<br/>· Low-performance home ONTs<br/>· Enterprise firewalls / behavior management devices<br/>· Gateways with special ICMP handling"]
    D --> E["Remove / bypass problematic devices<br/>Make the link as close to 'bare network' as possible"]
    E --> A
```

1. **Perform ICMP / UDP comparative test**
   - Use `ping -f` or other ICMP flood methods to observe ICMP responsiveness and packet loss
   - Use `iperf3 -u -c ...` for UDP performance comparison testing
   - If ICMP stability, throughput, or latency is **significantly better than UDP**, the link is worth deploying

2. **If ICMP performance is not as expected**
   This indicates there may be interfering factors in the link. Common issues include:
   - Low-performance, poorly implemented home ONTs (try switching the ONT to bridge mode and have the router handle dialing to mitigate ICMP rate limiting)
   - Various "enterprise-grade" firewalls, internet behavior management devices, or other gateways that specially handle ICMP

   > ⚠️ Such devices sometimes have underlying filtering, rate limiting, or policy processing logic that cannot be truly disabled — even when the web interface shows "firewall is off". It looks turned off, but it actually isn't.

3. **Re-test after removing intermediate devices**
   Remove problematic ONTs and bypass relevant firewalls as much as possible to bring the test link close to a "bare network" state, then repeat Step 1. Only after eliminating the influence of intermediate devices can you accurately determine whether the link is suitable for deployment.

---

## 💻 Operating System Requirements and Dependencies

<details open>
<summary><b>Ubuntu</b>（≥ 20.04, recommended 24.04 LTS and above）</summary>

```sh
sudo apt install -y git libbpf-dev clang llvm cmake libsodium-dev dkms \
    linux-tools libsodium-dev libelf-dev flex bison pkg-config
```

> 💡 If you are not using the standard `ubuntu` kernel, please install the corresponding `linux-tools` for your kernel, or build `bpftool` from source with `-DUSE_SYSTEM_LIBBPF_BPFTOOL=0`.

</details>

<details>
<summary><b>Arch Linux</b>（latest）</summary>

```sh
sudo pacman -S git base-devel libbpf clang cmake libsodium dkms flex bison pkg-config
```

</details>

<details>
<summary><b>OpenWrt</b></summary>

For OpenWrt users, we recommend using the kmod version [tutuicmptunnel-kmod](https://github.com/hrimfaxi/tutuicmptunnel-kmod),
which runs on OpenWrt **without requiring kernel recompilation**.

If you still insist on using the BPF version, please refer to the 📖 [OpenWrt Guide](docs/openwrt.md).

</details>

---

## 📥 Installation

### 1️⃣ Clone the code and compile

```sh
git clone https://github.com/hrimfaxi/tutuicmptunnel
cd tutuicmptunnel
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_HARDEN_MODE=1 -DUSE_SYSTEM_LIBBPF_BPFTOOL=1 .
make
sudo make install
```

> 💡 For peak performance, add `-DENABLE_XDP_INGRESS=1`, but note that `tcpdump` will only capture the transformed UDP traffic.
> Ubuntu 20.04 requires git-version libbpf/bpftool and disabled bpf timer: `-DUSE_SYSTEM_LIBBPF_BPFTOOL=0 -DDISABLE_BPF_TIMER=1`

### 2️⃣ Install the checksum fixup module

Both server and client should install [tutu_csum_fixup](tutu_csum_fixup/README.md) — a lightweight kretprobe kernel module
that fixes ICMP checksum issues that eBPF cannot modify (causes bad checksum on NICs with TX checksum offload enabled).

```sh
cd tutu_csum_fixup
sudo dkms remove tutu_csum_fixup/x.x --all  # remove old version if any
sudo make dkms
sudo tee -a /etc/modules-load.d/modules.conf <<< tutu_csum_fixup
sudo modprobe tutu_csum_fixup
```

> 💡 Some systems require setting the `force_sw_checksum` parameter. See [tutu_csum_fixup documentation](tutu_csum_fixup/README.md#force_sw_checksum).
> Alternatively, disable TX checksum offload directly with `ethtool -K eth0 tx off` (but loses hardware acceleration).

### 3️⃣ Server: Set up systemd service and enable optional `tuctl_server`

`tuctl_server` allows clients to remotely control server-side configuration. Before using, please note:

- 🔑 To prevent brute-force attacks, choose a sufficiently strong `PSK` (recommended to generate with `uuidgen -r`)
- 🕒 Since timestamp verification is used, both server and client need accurate system time (NTP synchronization)

```sh
# Auto-load tutuicmptunnel service on boot and restore configuration
sudo cp contrib/etc/systemd/system/tutuicmptunnel-server@.service /etc/systemd/system/
sudo systemctl enable --now tutuicmptunnel-server@eth0.service  # eth0 is the server network interface

# Optional tuctl_server
sudo cp contrib/etc/systemd/system/tutuicmptunnel-tuctl-server.service /etc/systemd/system/
sudo vim /etc/systemd/system/tutuicmptunnel-tuctl-server.service  # Edit psk, port, etc.
timedatectl | grep "System clock synchronized:"  # Verify system time is NTP synchronized
sudo systemctl daemon-reload
sudo systemctl enable --now tutuicmptunnel-tuctl-server
```

Now you can use `tuctl` to check the server status:

```console
$ sudo tuctl
tutuicmptunnel: Role: Server, BPF build type: Release, no-fixup: off

Peers:
....
```

<details>
<summary>👉 Start server mode manually without systemd</summary>

```sh
sudo tuctl unload iface eth0                  # Clean up first
sudo tuctl load iface eth0                    # Load BPF program onto eth0
sudo tuctl server                             # Set to server mode
sudo tuctl server-add uid 123 address 1.2.3.4 port 1234  # Add client
sudo tuctl server-del uid 123                 # Delete client
```

</details>

### 4️⃣ Optional: Set up UID and hostname mapping table

For easier management, `tutuicmptunnel` supports mapping hostnames to `UIDs` via `/etc/tutuicmptunnel/uids`:

```sh
sudo mkdir -p /etc/tutuicmptunnel
sudo vim /etc/tutuicmptunnel/uids
```

File format:

```text
#
# Format: UID hostname # optional comment
#

0 alice # alice's laptop
1 bob   # bob's laptop
```

Once configured, all places in the `tuctl` command that require specifying a `UID` (e.g. `uid 0`) can use the hostname directly (e.g. `user alice`) instead, making management more intuitive.

### 5️⃣ Client: Set up systemd service and enable

```sh
sudo cp contrib/etc/systemd/system/tutuicmptunnel-client@.service /etc/systemd/system/
sudo systemctl enable --now tutuicmptunnel-client@enp4s0  # enp4s0 is your network interface
```

---

## 🚀 Quick Start

Client configuration example:

```sh
export ADDRESS=yourserver.com   # Server domain or IP
export PORT=3322                # UDP port to be converted to ICMP
export TUTU_UID=123             # tutuicmptunnel user ID
export PSK=yourlongpsk          # PSK for tuctl_server
export SERVER_PORT=14801        # Port for tuctl_server
export COMMENT=yourname         # Client identity description (visible in server tuctl output)

# Set to client mode
sudo tuctl client
# Add server endpoint configuration
sudo tuctl client-add uid $TUTU_UID address $ADDRESS port $PORT
# Verify configuration
sudo tuctl status
# Use tuctl_client to notify the server to sync new configuration
tuctl_client psk $PSK server $ADDRESS server-port $SERVER_PORT \
    <<< "server-add uid $TUTU_UID address @client_ip@ port $PORT comment $COMMENT"
```

Then confirm on the server that the rule has taken effect:

```console
$ sudo tuctl
tutuicmptunnel: Role: Server, BPF build type: Release, no-fixup: off

Peers:
  User: xxxx, Address: xxx.xxx.xxx.xxx, Sport: 37926, Dport: 3322, ICMP: 11403, Comment: yourname
```

✅ Once everything is ready: `UDP` traffic sent by the client to port `3322` will be automatically encapsulated as `ICMP`; the server receives and restores it to `UDP` for continued forwarding. **During the entire transmission, intermediate nodes can only see `ICMP Echo` / `Reply` packets.**

---

## 🧰 Main Use Cases

| Name | Description |
| :--- | :--- |
| [shadowquic](docs/shadowquic.md) | **🎯 Officially Recommended Best Companion** — Rust-based QUIC proxy, optimized for unstable and high packet-loss networks, perfectly complementing ICMP tunneling |
| [hysteria](docs/hysteria.md) | Proxy tool based on the QUIC protocol, optimized for unstable and high packet-loss networks |
| [iperf3](docs/iperf3.md) | Powerful network performance testing tool for measuring bandwidth, jitter, and packet loss |
| [xray + hysteria](docs/xray_hysteria.md) | Combination of Xray core and Hysteria protocol for accelerating and stabilizing network connections |
| [xray + kcptun](docs/xray_kcptun.md) | Combination of Xray core and KCPTun protocol for accelerating and stabilizing network connections |
| [xray + mkcp](docs/xray_mkcp.md) | Xray core with its built-in mKCP implementation for accelerating and stabilizing network connections |
| [xray + dns](docs/xray_dns.md) | Forward DNS on VPS using xray-core (dokodemo-door), and encapsulate UDP as ICMP on the OpenWrt side for transmission |
| [wireguard](docs/wireguard.md) | Modern, high-performance, and easy-to-configure secure VPN tunnel |
| [openwrt](docs/openwrt.md) | Highly customizable Linux operating system for embedded devices (especially routers) |
| [mosh](docs/mosh.md) | SSH-initiated, UDP-transmitted roaming shell that supports reconnection and maintains smooth interaction under high-latency/jitter networks |

---

## 🙏 Acknowledgements

During the design, implementation, and performance tuning of `tutuicmptunnel`, we have referenced and benefited from numerous excellent open-source projects and technical articles. We sincerely thank their authors and community contributors!

- [hysteria](https://github.com/apernet/hysteria)
- [shadowquic](https://github.com/spongebob888/shadowquic)
- [kcptun](https://github.com/xtaci/kcptun)
- [xray-core](https://github.com/XTLS/Xray-core)
- [udp2raw](https://github.com/wangyu-/udp2raw)
- [mimic](https://github.com/hack3ric/mimic)
- [libbpf-bootstrap](https://github.com/libbpf/libbpf-bootstrap)

Special thanks to [@hack3ric](https://github.com/hack3ric) and all contributors for their continuous maintenance, making `UDP` → `fakeTCP` obfuscation on eBPF possible.

---

## 📄 License

This project as a whole is licensed under the **GNU General Public License v2.0 or later** (`GPL-2.0-or-later`).
The `libbpf` and `bpftool` submodules retain their respective original licenses (`LGPL-2.1` / `BSD-2-Clause`).
