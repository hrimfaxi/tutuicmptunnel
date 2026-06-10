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
 * session_key 语义约定：
 * - address: 客户端地址, 统一以 IPv6 存储；IPv4 映射为 v4-mapped IPv6
 * - sport/dport: 客户端源/目的端口, 网络字节序
 * - 在 server 模式下，该 key 用于从 UDP 回包查找所属 ICMP 客户端
 */
struct session_key {
  struct in6_addr address;
  __be16          sport;
  __be16          dport;
};

/*
 * session_value 语义约定：
 * - age: 自系统启动以来的秒数（bpf_ktime_get_ns() / NS_PER_SEC）
 * - uid: 所属用户的 UID
 * - client_sport: 客户端原始源端口（网络字节序），用于 ICMP seq 重建
 */
struct session_value {
  __u64  age;
  __u8   uid;
  __be16 client_sport;
};

/*
 * user_info 语义约定（仅 server 模式使用）：
 * - address: 客户端源地址，统一为 IPv6（v4-mapped）
 * - icmp_id: 客户端当前使用的 ICMP ID（网络字节序）
 * - dport: 服务器上为该客户端分配的 UDP 目的端口（网络字节序）
 * - comment: 纯文本备注，不以 null 结尾时由打印侧限制长度
 */
struct user_info {
  struct in6_addr address;
  __be16          icmp_id;
  __be16          dport;
  __u8            comment[22];
};

/*
 * egress_peer_key 语义约定（client 模式，出向查找）：
 * - address: 服务器地址，统一为 IPv6（v4-mapped IPv4）
 * - port: 服务器 UDP 端口，网络字节序
 */
struct egress_peer_key {
  struct in6_addr address;
  __be16          port;
};

/*
 * egress_peer_value 语义约定：
 * - uid: 该服务器为该客户端分配的 UID
 * - comment: 纯文本备注，打印侧限制长度
 */
struct egress_peer_value {
  __u8 uid;         // UID for this client
  __u8 comment[22]; // Comment
};

/*
 * ingress_peer_key 语义约定（client 模式，入向查找）：
 * - address: 服务器地址，统一为 IPv6（v4-mapped IPv4）
 * - uid: 服务器返回 ICMP 报文 code 字段携带的 UID
 */
struct ingress_peer_key {
  struct in6_addr address; // Server address in network byte order
  __u8            uid;     // UID (icmp id)
};

/*
 * ingress_peer_value 语义约定：
 * - port: 客户端本地期望接收的 UDP 源端口（网络字节序），
 *         server 回包时以此重建 UDP 源端口
 */
struct ingress_peer_value {
  __be16 port;
};

/*
 * config 语义约定：
 * - packets_processed/dropped/checksum_errors/fragmented/gso:
 *     自加载以来的累计计数器，由 eBPF 侧原子递增
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
  __u8  no_fixup;  // 1 if not using bpf_skb_change_type hack
  __u8  is_server; // 1 if server mode, 0 if client mode
};

/*
 * gc_switch 语义约定：
 * - enabled: 1 允许 GC 定时器继续调度，0 则停止并退出
 *   用户态通过 gc_switch_map 写入，控制 BPF 侧 session_map 清理
 */
struct gc_switch {
  __u8 enabled;
};

// vim: set sw=2 ts=2 expandtab:
