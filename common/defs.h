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

enum {
  LINK_NONE,
  LINK_ETH,
  LINK_AUTODETECT,
};

// 客户端地址/源端口/目的端口到UID的映射
// 服务器上用于查找udp回包属于哪个客户端
// 使用tuctl reaper命令定期删除过期项目
struct session_key {
  struct in6_addr address; // 客户端地址
  __be16          sport;   // 客户端源端口
  __be16          dport;   // 客户端目的端口
};

struct session_value {
  __u64  age;
  __u8   uid;
  __be16 client_sport;
};

// User struct to store in the map
struct user_info {
  struct in6_addr address;     // IP address in network byte order
  __be16          icmp_id;     // ICMP ID
  __be16          dport;       // Destination port
  __u8            comment[22]; // Comment
};

struct egress_peer_key {
  struct in6_addr address; // Server address in network byte order
  __be16          port;    // Server port
};

struct egress_peer_value {
  __u8 uid;         // UID for this client
  __u8 comment[22]; // Comment
};

struct ingress_peer_key {
  struct in6_addr address; // Server address in network byte order
  __u8            uid;     // UID (icmp id)
};

struct ingress_peer_value {
  __be16 port; // Server UDP Port
};

// Config struct
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

struct gc_switch {
  __u8 enabled;
};

/* vim: set sw=2 expandtab: */
