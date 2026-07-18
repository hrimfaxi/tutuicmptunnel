[English](./README.md) | [简体中文](./README_zh-CN.md)

---

# tutu_csum_fixup — Checksum Fixup Kernel Module

This module hooks into the error return path of `bpf_skb_change_type()` via **kretprobe** to recalculate ICMP/ICMPv6 checksums for `tutuicmptunnel` (BPF version) on devices with **hardware TX checksum offload** enabled, preventing `bad checksum` packets.

Since eBPF cannot disable or modify hardware offload, this work must be handled by a kernel module. It is recommended to load this module automatically at boot to avoid checksum issues.

## Why Is It Needed?

Many NICs / switch chips have **TX checksum offload** enabled by default:

```bash
ethtool -k eth0 | grep checksum
# tcp-segmentation-offload: on
# tx-checksum-ip-generic: on
```

When the eBPF program performs **UDP ⇆ ICMP** protocol transformation, Linux currently **has no public API for BPF to disable or modify hardware offload**. The result is:

- After UDP → ICMP conversion, the ICMP packet is sent before the hardware recalculates the checksum → bad cksum
- Upper-layer applications report errors, and packet captures show "incorrect icmp checksum"

There are two solutions:

1. **Load this module** (recommended): capture the `skb` in the error path and manually fix the checksum.

   > [!WARNING]
   > Official OpenWrt kernels disable kprobes by default; you must compile a custom kernel to use this module.

2. **Completely disable TX checksum offload on the device**:

   ```bash
   ethtool -K eth0 tx-checksum-ip-generic off tx-checksum-ipv4 off tx off
   ```

   Disadvantages:

   - Some NICs / DSA switch chips do not support disabling offload.
   - All traffic loses hardware acceleration, increasing CPU usage.

## Installation

### Quick Install (DKMS, for mainline distribution kernels)

```bash
sudo apt install dkms -y          # Ubuntu / Debian
sudo pacman -S dkms               # Arch Linux

cd tutu_csum_fixup
sudo make dkms-remove; sudo make dkms
sudo tee /etc/modules-load.d/tutu_csum_fixup.conf <<< tutu_csum_fixup
sudo modprobe tutu_csum_fixup
```

If `tutu_csum_fixup` successfully rewrote the packet, you can see in the kernel log:

```bash
dmesg | grep tutu_csum_fixup
# tutu_csum_fixup: magic called
```

### Manual Build

```bash
cd tutu_csum_fixup
make                        # Build tutu_csum_fixup.ko for the running kernel
sudo make modules_install
sudo modprobe tutu_csum_fixup
```

Optional parameters: `KSRC=<kernel source tree>`, `KVER=<version>`, `LLVM=1`.

### Cross-Compilation Example (OpenWrt)

```bash
make -C tutu_csum_fixup \
    KSRC=/path/to/linux-6.6.86 \
    CROSS_COMPILE=/path/to/aarch64-openwrt-linux- \
    ARCH=arm64
```

## Module Parameters

### `force_sw_checksum`

For certain special environments (such as servers that do not support ICMP hardware checksums, e.g., Alibaba Cloud instances or some QEMU e1000e NIC scenarios), you need to force software checksum calculation. Configure as follows:

```bash
sudo tee /etc/modprobe.d/tutu_csum_fixup.conf <<< "options tutu_csum_fixup force_sw_checksum=1"
sudo modprobe -r tutu_csum_fixup; sudo modprobe tutu_csum_fixup
```

| Parameter | Description | Default |
| ---- | ---- | ------ |
| `force_sw_checksum` | Force software checksum calculation for environments where hardware checksum is unavailable or unreliable. | `0` (disabled) |
