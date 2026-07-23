README.md
==========

TUTU-ICMP-Tunnel BPF 程序
——————————————

该目录中的 `tutuicmptunnel.bpf.c`（内含 egress 与 ingress 两个 SEC）把 **UDP** 报文就地转换成 **ICMP / ICMPv6 Echo**，或反向还原为 UDP，实现“把任意 UDP 流量伪装成 ping” 的通道功能。
配合可选的 `tutu_csum_fixup` kprobe 模块，可在 **开启 TX-checksum offload 且无法关闭** 的网卡上自动修复校验和。服务器模式支持多用户映射，客户端模式支持定时滚动 ICMP-ID 。

------------------------------------------------------------------------------

快速开始
--------

### 1. 编译

目录已提供 `Makefile`，直接执行：

```bash
make            # 生成 release 与 debug 两套 .o 及 .skel.h
```

可选变量
• `USE_BTF=0|1` 是否启用 BTF（默认 1，需要内核 `/sys/kernel/btf/vmlinux`）
• `CLANG=/path/clang`、`BPFTOOL=/path/bpftool` 指定工具链
• `V=1` 输出详细编译命令

生成物
```
tutuicmptunnel.bpf.o         # release 版
tutuicmptunnel.debug.bpf.o   # 带 bpf_printk 的调试版
tutuicmptunnel.skel.h        # libbpf skeleton
tutuicmptunnel.debug.skel.h
```

### 2. 加载到 tc

```bash
DEV=eth0
tc qdisc add dev $DEV clsact           # 如已存在跳过
tc filter add dev $DEV egress  bpf da obj tutuicmptunnel.bpf.o sec 'tc/egress'
tc filter add dev $DEV ingress bpf da obj tutuicmptunnel.bpf.o sec 'tc/ingress'
```

卸载：

```bash
tc qdisc del dev $DEV clsact
```

### 3. （可选）校验和修复模块

若网卡无法关闭 TX checksum offload：

```bash
cd ../tutu_csum_fixup
sudo make dkms        # 或 make && sudo insmod tutu_csum_fixup.ko
```

随后在用户态将 `config_map.use_fixup` 置 1（`tuctl ... fixup`）。

------------------------------------------------------------------------------

核心原理
--------

1. **L2/L3/L4 解析**
   `parse_headers()` 自动探测有无以太网头并递归跳过 IPv6 扩展头，返回 L4 协议及偏移量。
2. **TC egress (UDP → ICMP)**
   • 仅处理 UDP 包，拒绝 GSO/分片。
   • 根据运行模式确定 ICMP type/code/id/seq：
     – server：Echo-Reply；code = UID（多用户）
     – client：Echo-Request；code = UID
   • 用纯 BPF 算法或 `MAGIC_FLAG3` + kprobe 重算校验和。
   • `bpf_l3_csum_replace()` 增量修正 IPv4 头校验和。
3. **TC ingress (ICMP → UDP)**
   • 仅接收 Echo-Request/Reply。校验 UID，客户端对源地址做过滤。
   • 根据 ICMP 伪头/载荷反推 UDP 载荷校验和并生成完整 UDP 校验和。
   • 更新 `session_map`（服务器）或检查 `peer_map`（客户端）。
4. **数据面存储**
   • `session_map`  (LRU_HASH)   : <addr,sport,dport> → {uid,age}
   • `user_map`     (HASH)       : uid → {addr,icmp_id,sport,dport,…}
   • `peer_map`     (ARRAY[1])   : 客户端保存服务器信息
   • `config_map`   (ARRAY[1])   : 统计与运行时开关
5. **异常处理**
   GSO、IP 分片、硬件 offload、MIPS 非对齐访问等均显式处理或丢弃。

------------------------------------------------------------------------------

目录结构
--------

```
bpf/
 ├── Makefile                    # 一键编译脚本
 ├── tutuicmptunnel.bpf.c        # 主 eBPF 程序
 ├── defs.h                      # 共享结构体定义
 ├── try.h                       # 错误处理 / 取指针宏
 └── tutuicmptunnel.*.skel.h     # 由 bpftool 生成的 skeleton
common/
 └── (同级) defs.h try.h         # 被包含
tutu_csum_fixup/                 # 可选 kprobe 校验和修复模块
```

------------------------------------------------------------------------------

调试 & 统计
-----------

* `make DEBUG=1` 或直接使用 `tutuicmptunnel.debug.bpf.o`，`bpf_printk()` 日志可在 `dmesg` 查看。
* `bpftool map dump id <id>` 查看 `config_map` 里的计数器：
  `packets_processed / packets_dropped / gso_packets / fragmented_packets …`

------------------------------------------------------------------------------

许可证
------

本 BPF 程序与附带源码均遵循 GNU General Public License v2.0 或更高版本 (`GPL-2.0-or-later`)。
