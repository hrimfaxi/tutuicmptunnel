# tutu_csum_fixup —— 校验和修复内核模块

本模块通过 **kretprobe** 挂接到 `bpf_skb_change_type()` 的错误返回路径，为 `tutuicmptunnel`（eBPF 版本）在**开启硬件 TX 校验和 offload** 的设备上重新修正 ICMP/ICMPv6 校验和，避免出现 `bad checksum` 报文。

由于 eBPF 无法关闭或修改硬件 offload，这部分工作只能由内核模块接管。建议开机自动加载本模块，以免遇到校验和问题。

## 为什么需要它？

许多网卡 / 交换芯片默认开启 **TX checksum offload**：

```bash
ethtool -k eth0 | grep checksum
# tcp-segmentation-offload: on
# tx-checksum-ip-generic: on
```

当 eBPF 程序执行 **UDP ⇆ ICMP** 的协议变换时，Linux 目前**没有公开 API 可供 BPF 关闭或修改硬件 offload**。结果就是：

- UDP → ICMP 转换后，ICMP 报文在硬件重新计算校验和前就被发出 → bad cksum
- 上层应用报错，抓包能看到 "incorrect icmp checksum"

解决方法有两种：

1. **加载本模块**（推荐）：在错误路径中抓取 `skb` 并手动修正校验和。

   > [!WARNING]
   > OpenWrt 官方内核默认禁用 kprobes，需自行编译内核才能使用本模块。

2. **彻底关闭设备的 TX checksum offload**：

   ```bash
   ethtool -K eth0 tx-checksum-ip-generic off tx-checksum-ipv4 off tx off
   ```

   缺点：

   - 某些网卡 / DSA 交换芯片不支持关闭。
   - 所有流量都失去硬件加速，CPU 占用上升。

## 安装

### 快速安装（DKMS，适用于主线发行版内核）

```bash
sudo apt install dkms -y          # Ubuntu / Debian
sudo pacman -S dkms               # Arch Linux

cd tutu_csum_fixup
sudo make dkms-remove; sudo make dkms
sudo tee /etc/modules-load.d/tutu_csum_fixup.conf <<< tutu_csum_fixup
sudo modprobe tutu_csum_fixup
```

如果 `tutu_csum_fixup` 成功重写了报文，可以在内核日志中看到：

```bash
dmesg | grep tutu_csum_fixup
# tutu_csum_fixup: magic called
```

### 手动编译

```bash
cd tutu_csum_fixup
make                        # 针对当前运行内核生成 tutu_csum_fixup.ko
sudo make modules_install
sudo modprobe tutu_csum_fixup
```

可选参数：`KSRC=<内核源码树>`、`KVER=<版本>`、`LLVM=1`。

### 交叉编译示例（OpenWrt）

```bash
make -C tutu_csum_fixup \
    KSRC=/path/to/linux-6.6.86 \
    CROSS_COMPILE=/path/to/aarch64-openwrt-linux- \
    ARCH=arm64
```

## 模块参数

### `force_sw_checksum`

对于某些特殊环境（如部分不支持 ICMP 硬件校验和的服务器，例如阿里云实例或某些 QEMU e1000e 网卡场景），需要强制使用软件方式计算校验和。可通过如下方式配置：

```bash
sudo tee /etc/modprobe.d/tutu_csum_fixup.conf <<< "options tutu_csum_fixup force_sw_checksum=1"
sudo modprobe -r tutu_csum_fixup; sudo modprobe tutu_csum_fixup
```

| 参数 | 说明 | 默认值 |
| ---- | ---- | ------ |
| `force_sw_checksum` | 强制使用软件方式计算校验和，用于硬件校验和不可用或不可靠的环境。 | `0`（关闭） |
