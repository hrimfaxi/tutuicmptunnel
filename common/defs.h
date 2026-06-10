#pragma once

#ifdef __BPF_USE_BTF__
#if defined(__KERNEL__) || defined(__BPF__)
#include "vmlinux.h"
#else
#include <stdint.h>
#ifndef __u8
typedef uint8_t __u8;
#endif
#ifndef __u16
typedef uint16_t __u16;
#endif
#ifndef __u32
typedef uint32_t __u32;
#endif
#ifndef __be16
typedef uint16_t __be16;
#endif
#ifndef __be32
typedef uint32_t __be32;
#endif
#endif
#else
#include <linux/types.h>
#endif

#define CSUM_MANGLED_0 ((__sum16) 0xffff)

enum {
  LINK_NONE,
  LINK_ETH,
  LINK_AUTODETECT,
};

/*
 * session_key: server 模式下，用于从 UDP 回包定位所属 ICMP 会话
 * - address: 客户端地址，统一 IPv6；IPv4 以 v4-mapped 形式存储
 * - sport: 客户端 ICMP ID（可能被 NAT 改写后的值），网络字节序。
 *          NAT 设备通常改写 icmp_id 但不动 icmp_seq，因此服务器
 *          用 NAT 后的 icmp_id 作为查找键，才能匹配 UDP 回包的目的端口。
 * - dport: 服务器上使用的 UDP 端口（被 ICMP 化保护）
 *
 * 注：sport/dport 的命名源于键值构造时的位置约定，并非严格对应
 *     报文方向的源/目的。构造详见 handle_egress() 与 update_session_map()。
 */
struct session_key {
  struct in6_addr address;
  __be16          sport;
  __be16          dport;
};

/*
 * session_value: server 模式下会话的元数据
 * - age: 上次活跃时间戳（秒，基于 bpf_ktime_get_ns() / NS_PER_SEC）
 * - uid: 该会话所属用户的 UID
 * - client_sport: 客户端原始 UDP 源端口（网络字节序）。
 *                 客户端初始设置 icmp_id = icmp_seq = 源端口；NAT 设备
 *                 改写 icmp_id 后，服务器在 ingress 侧把原始值（icmp_seq）
 *                 存入本字段。回包时复用为 ICMP seq，NAT 不碰 seq，因此
 *                 客户端收到后可完美还原为原始 UDP 源端口。
 *                 同时 user->icmp_id 在 ingress 时动态更新为 NAT 改写后的
 *                 新值，该值被用作 UDP 源端口（见 handle_ingress），因此
 *                 UDP 回包的目的端口即为此值，保证 egress 侧 session_map
 *                 查找始终有效。
 */
struct session_value {
  __u64  age;
  __u8   uid;
  __be16 client_sport;
};

/*
 * user_info: server 模式下，每个授权客户端的静态配置
 * - address: 客户端源地址，统一 IPv6（v4-mapped IPv4）
 * - icmp_id: 客户端当前使用的 ICMP Echo ID（网络字节序）
 *            动态更新：ingress 时若发现新值，立即覆盖旧值。
 *            注意：多源端口并发时，该字段会被最后一次通信的 icmp_id
 *            覆盖，但这不影响正确性——真正区分多源端口的是 session_map
 *            的独立条目，user->icmp_id 仅用于 NAT 改写后的快速跟踪。
 * - dport: 服务器上为该客户端分配的 UDP 目的端口（网络字节序）
 * - comment: 纯文本备注，不以 '\0' 结尾时由打印侧限制长度
 */
struct user_info {
  struct in6_addr address;
  __be16          icmp_id;
  __be16          dport;
  __u8            comment[22];
};

/*
 * egress_peer_key: client 模式下，出向报文（客户端→服务器）的查找键
 * - address: 服务器地址，统一 IPv6（v4-mapped IPv4）
 * - port: 服务器 UDP 端口，网络字节序
 */
struct egress_peer_key {
  struct in6_addr address;
  __be16          port;
};

/*
 * egress_peer_value: client 模式下，出向 peer 的附加属性
 * - uid: 服务器为该客户端分配的 UID
 * - comment: 纯文本备注
 */
struct egress_peer_value {
  __u8 uid;         // UID for this client
  __u8 comment[22]; // Comment
};

/*
 * ingress_peer_key: client 模式下，入向报文（服务器→客户端）的查找键
 * - address: 服务器地址，统一 IPv6（v4-mapped IPv4）
 * - uid: 服务器返回 ICMP 报文 code 字段携带的 UID
 */
struct ingress_peer_key {
  struct in6_addr address; // Server address in network byte order
  __u8            uid;     // UID (icmp id)
};

/*
 * ingress_peer_value: client 模式下，入向 peer 的附加属性
 * - port: 客户端本地期望的 UDP 源端口（网络字节序），
 *         server 回包时以此重建 UDP 头部
 */
struct ingress_peer_value {
  __be16 port;
};

/*
 * config: eBPF 全局配置与统计（单元素 array map，key 恒为 0）
 * - packets_processed/dropped/checksum_errors/fragmented/gso:
 *     自加载以来的累计计数器，由 eBPF 侧 __sync_fetch_and_add 更新
 * - session_max_age: 会话最大存活时间（秒），与 bpf_ktime_get_ns 对齐
 * - no_fixup: 1 表示不使用 bpf_skb_change_type 内核模块校验和修复
 * - is_server: 1 为 server 模式，0 为 client 模式
 */
struct config {
  __u64 packets_processed;
  __u64 packets_dropped;
  __u64 checksum_errors;
  __u64 fragmented;
  __u64 gso;
  __u32 session_max_age;
  __u8  no_fixup;
  __u8  is_server;
};

/*
 * gc_switch: 控制 BPF 侧 session_map 垃圾回收定时器
 * - enabled: 1 允许 GC 继续调度；0 则停止并退出
 *   用户态通过 gc_switch_map 写入，handle_gc_timer 读取
 */
struct gc_switch {
  __u8 enabled;
};

// vim: set sw=2 ts=2 expandtab:
