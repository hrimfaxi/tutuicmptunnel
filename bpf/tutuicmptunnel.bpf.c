#ifdef __TARGET_ARCH_mips
#define __mips__
#endif

#ifdef __BPF_USE_BTF__
#include "defs.h"

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define TC_ACT_OK   0
#define TC_ACT_SHOT 2
#define XDP_DROP    1
#define XDP_PASS    2
#define ETH_P_IP    0x0800 /* Internet Protocol packet	*/
#define ETH_P_IPV6  0x86DD /* IPv6 over bluebook	*/
#define ETH_P_IPV6  0x86DD /* IPv6 over bluebook	*/
#define ETH_HLEN    14     /* Total octets in header.	*/
#else
#include <linux/bpf.h>

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
#include <linux/icmp.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/pkt_cls.h>
#include <linux/udp.h>

#include "defs.h"
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef EBUSY
#define EBUSY 16
#endif

#include "try.h"

#define MIMIC_BPF 1

#include "../tutu_csum_fixup/csum-hack.h"

#ifdef BPF_DEBUG
#if defined(__clang_major__) && __clang_major__ >= 16
#define TUTU_LOG(fmt, ...) bpf_printk(fmt, ##__VA_ARGS__)
#else
// #warning your clang is too old to enable bpf_printk!
#define TUTU_LOG(fmt, ...)                                                                                                     \
  do {                                                                                                                         \
  } while (0)
#endif
#else
#define TUTU_LOG(fmt, ...)                                                                                                     \
  do {                                                                                                                         \
  } while (0)
#endif

#include "net_proto.h"

// Map to look up UID by IP:port
struct {
  __uint(type, BPF_MAP_TYPE_LRU_HASH);
  __type(key, struct session_key);
  __type(value, struct session_value); // UID
  __uint(max_entries, 16384);
} session_map SEC(".maps");

// Map to store users (for server mode)
struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __type(key, __u8); // UID
  __type(value, struct user_info);
  __uint(max_entries, 256);
} user_map SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __type(key, struct egress_peer_key);
  __type(value, struct egress_peer_value);
  __uint(max_entries, 1024);
} egress_peer_map SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __type(key, struct ingress_peer_key);
  __type(value, struct ingress_peer_value);
  __uint(max_entries, 1024);
} ingress_peer_map SEC(".maps");

// Configuration map
struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __type(key, __u32); // Always key 0
  __type(value, struct config);
  __uint(max_entries, 1);
} config_map SEC(".maps");

// 0 - release, 1 - debug
struct {
  __uint(type, BPF_MAP_TYPE_ARRAY);
  __uint(max_entries, 1);
  __type(key, __u32);
  __type(value, __u32);
} build_type_map SEC(".maps");

#ifndef DISABLE_BPF_TIMER
struct gc_timer {
  struct bpf_timer timer;
};

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 1);
  __type(key, __u32);
  __type(value, struct gc_timer);
} gc_timer_map SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_HASH);
  __uint(max_entries, 1);
  __type(key, __u32);
  __type(value, struct gc_switch);
} gc_switch_map SEC(".maps");
#endif

#define NS_PER_SEC 1000000000ULL

#define IPPROTO_HOPOPTS  0
#define IPPROTO_ROUTING  43
#define IPPROTO_FRAGMENT 44
#define IPPROTO_ICMPV6   58
#define IPPROTO_NONE     59
#define IPPROTO_DSTOPTS  60
#define IPPROTO_MH       135

static __always_inline int bpf_memcmp(const void *s1, const void *s2, size_t n) {
#if defined(__clang_major__) && __clang_major__ >= 15
  return __builtin_memcmp(s1, s2, n);
#else
  const __u8 *p1 = (typeof(p1)) s1;
  const __u8 *p2 = (typeof(p2)) s2;

#pragma unroll
  for (size_t i = 0; i < n; i++) {
    if (p1[i] != p2[i]) {
      return (int) p1[i] - (int) p2[i];
    }
  }
  return 0;
#endif
}

#ifdef __mips__
static __always_inline int bpf_volatile_memcmp(const volatile void *s1, const volatile void *s2, size_t n) {
  const volatile __u8 *p1 = (const volatile __u8 *) s1;
  const volatile __u8 *p2 = (const volatile __u8 *) s2;

#pragma unroll
  for (size_t i = 0; i < n; i++) {
    __u8 a = p1[i];
    __u8 b = p2[i];
    if (a != b)
      return (int) a - (int) b;
  }
  return 0;
}
#else
static __always_inline int bpf_volatile_memcmp(const volatile void *s1, const volatile void *s2, size_t n) {
  return __builtin_memcmp((const void *) s1, (const void *) s2, n);
}
#endif

static __always_inline void *skb_data_end(const struct __sk_buff *ctx) {
  void *data_end;
  asm volatile("%[res] = *(u32 *)(%[base] + %[offset])"
               : [res] "=r"(data_end)
               : [base] "r"(ctx), [offset] "i"(offsetof(struct __sk_buff, data_end)), "m"(*ctx));
  return data_end;
}

static __always_inline void *xdp_data_end(const struct xdp_md *ctx) {
  void *data_end;
  asm volatile("%[res] = *(u32 *)(%[base] + %[offset])"
               : [res] "=r"(data_end)
               : [base] "r"(ctx), [offset] "i"(offsetof(struct xdp_md, data_end)), "m"(*ctx));
  return data_end;
}

static inline bool ipv6_is_ext(__u8 nexthdr) {
  switch (nexthdr) {
  case IPPROTO_HOPOPTS:
  case IPPROTO_ROUTING:
  case IPPROTO_FRAGMENT:
  case IPPROTO_DSTOPTS:
  case IPPROTO_MH:
    return 1;
  default:
    return 0;
  }
}

// 打印8字节数据为16进制
static __always_inline void debug_hexdump_8(const char *label, const void *ptr) {
  (void) label;
#ifdef BPF_DEBUG
  const unsigned char *p = (const unsigned char *) ptr;
  (void) p;
  TUTU_LOG("%s: %02x %02x %02x %02x %02x %02x %02x %02x", label, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
#endif
}

static __always_inline void debug_hexdump_4(const char *label, const void *ptr) {
  (void) label;
#ifdef BPF_DEBUG
  const unsigned char *p = (const unsigned char *) ptr;
  (void) p;
  TUTU_LOG("%s: %02x %02x %02x %02x", label, p[0], p[1], p[2], p[3]);
#endif
}

static __always_inline __u32 get_unaligned(void *p) {
#ifdef __mips__
  __u32 val = 0;
  bpf_probe_read_kernel(&val, sizeof(val), p);
  return val;
#else
  return *(__u32 *) p;
#endif
}

static __always_inline __u64 get_unaligned64(void *p) {
#ifdef __mips__
  __u64 val = 0;
  bpf_probe_read_kernel(&val, sizeof(val), p);
  return val;
#else
  return *(__u64 *) p;
#endif
}

static __always_inline __sum16 csum_fold(__wsum csum) {
  __u32 sum = (__u32) csum;
  sum       = (sum & 0xffff) + (sum >> 16);
  sum       = (sum & 0xffff) + (sum >> 16);
  return (__sum16) ~sum;
}

static __always_inline __wsum csum_unfold(__sum16 n) {
  return (__wsum) n;
}

static __always_inline __wsum csum_add(__wsum csum, __wsum addend) {
  __u32 res = (__u32) csum;
  res += (__u32) addend;
  return (__wsum) (res + (res < (__u32) addend));
}

static inline __wsum csum_sub(__wsum csum, __wsum addend) {
  return csum_add(csum, ~addend);
}

static __always_inline __wsum udp_pseudoheader_sum(struct iphdr *iph, struct udphdr *udp) {
  __wsum sum;
  __be32 saddr = get_unaligned(&iph->saddr);
  __be32 daddr = get_unaligned(&iph->daddr);

  sum = (saddr >> 16);
  sum += (saddr & 0xFFFF);
  sum += (daddr >> 16);
  sum += (daddr & 0xFFFF);
  sum += bpf_htons(IPPROTO_UDP);
  sum += udp->len;

  return sum;
}

static __always_inline __wsum udpv6_pseudoheader_sum(struct ipv6hdr *ip6h, struct udphdr *udp) {
  __wsum sum;

  // UDP长度 (32位，高16位为0)
  // 高16位为0
  sum = udp->len; // 低16位

  // 协议号 (32位，高16位为0，低16位是协议号)
  // 高16位为0
  sum += bpf_htons(IPPROTO_UDP); // 低16位

  // IPv6源地址 (16字节 = 4个32位字)
#pragma unroll
  for (int i = 0; i < 4; i++) {
    sum = csum_add(sum, ip6h->saddr.in6_u.u6_addr32[i]);
  }

  // IPv6目的地址 (16字节 = 8个16位字)
#pragma unroll
  for (int i = 0; i < 4; i++) {
    sum = csum_add(sum, ip6h->daddr.in6_u.u6_addr32[i]);
  }

  return sum;
}

// icmp_len: icmp头部+icmp负载长度，主机字序
static __always_inline __wsum icmpv6_pseudoheader_sum(struct ipv6hdr *ip6h, __u32 icmp_len) {
  __wsum sum;
  // 需要转换为大端
  __be32 be_icmp_len = bpf_htonl(icmp_len);

  // ICMPv6长度，高16位+低16位
  sum = be_icmp_len >> 16;     // 高16位
  sum += be_icmp_len & 0xFFFF; // 低16位

  // 3字节0 + 下一个字节是协议号
  sum += bpf_htons(IPPROTO_ICMPV6);

  // 源地址
#pragma unroll
  for (int i = 0; i < 4; i++) {
    sum = csum_add(sum, ip6h->saddr.in6_u.u6_addr32[i]);
  }

  // 目的地址
#pragma unroll
  for (int i = 0; i < 4; i++) {
    sum = csum_add(sum, ip6h->daddr.in6_u.u6_addr32[i]);
  }

  return sum;
}

// 结果为大端
static __always_inline __wsum udp_header_sum(struct udphdr *udp) {
  return udp->source + udp->dest + udp->len;
}

#define s6_addr   in6_u.u6_addr8
#define s6_addr32 in6_u.u6_addr32

static __always_inline void ipv4_to_ipv6_mapped(__be32 ipv4, struct in6_addr *ipv6) {
#ifdef __mips__
  __builtin_memset(ipv6->in6_u.u6_addr8, 0, 10);
  ipv6->in6_u.u6_addr8[10] = 0xff;
  ipv6->in6_u.u6_addr8[11] = 0xff;

  __builtin_memcpy(ipv6->in6_u.u6_addr8 + 12, &ipv4, sizeof(ipv4));
#else
  ipv6->s6_addr32[0] = 0;
  ipv6->s6_addr32[1] = 0;
  ipv6->s6_addr32[2] = bpf_htonl(0x0000ffff);
  ipv6->s6_addr32[3] = ipv4;
#endif
}

static __always_inline void ipv6_copy(struct in6_addr *dst, const struct in6_addr *src) {
#ifdef __mips__
  __builtin_memcpy((void *) (volatile __u8 *) dst, (const void *) (volatile __u8 *) src, sizeof(*dst));
#else
  *dst = *src;
#endif
}

static __always_inline int ipv6_equal(const struct in6_addr *a, const struct in6_addr *b) {
#ifdef __mips__
  return bpf_volatile_memcmp((const volatile void *) a, (const volatile void *) b, sizeof(*a)) == 0;
#else
  return bpf_memcmp(a, b, sizeof(*a)) == 0;
#endif
}

static __always_inline void dump_skb(struct __sk_buff *skb) {
#ifdef BPF_DEBUG
  int len = skb->len;
  enum {
    DUMP_CHUNK = 8,
  };
  __u8 dump[DUMP_CHUNK];
  int  offset = 0;

  TUTU_LOG("skb->len: %u", len);

#pragma unroll
  for (int i = 0; i < 128; i++) {
    offset = i * DUMP_CHUNK;
    if (len <= offset)

      break;
    int chunk = len - offset > DUMP_CHUNK ? DUMP_CHUNK : len - offset;
    if (bpf_skb_load_bytes(skb, offset, dump, DUMP_CHUNK) < 0)
      break;
// 只打印 chunk 长度，不要超过 dump 长度
#pragma unroll
    for (int j = 0; j < DUMP_CHUNK; j++) {
      if (j < chunk)
        TUTU_LOG("skb[%d]=0x%02x", offset + j, dump[j]);
    }
  }

  // 2. 打印尾部零头（静态展开）
  int tail = len - offset;
  if (tail > 0 && tail < DUMP_CHUNK) {
    __u8 tail_dump[7]; // DUMP_CHUNK-1
    switch (tail) {
    case 7:
      if (bpf_skb_load_bytes(skb, offset, tail_dump, 7) == 0) {
        TUTU_LOG("skb[%d]=0x%02x", offset + 0, tail_dump[0]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 1, tail_dump[1]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 2, tail_dump[2]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 3, tail_dump[3]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 4, tail_dump[4]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 5, tail_dump[5]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 6, tail_dump[6]);
      }
      break;
    case 6:
      if (bpf_skb_load_bytes(skb, offset, tail_dump, 6) == 0) {
        TUTU_LOG("skb[%d]=0x%02x", offset + 0, tail_dump[0]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 1, tail_dump[1]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 2, tail_dump[2]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 3, tail_dump[3]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 4, tail_dump[4]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 5, tail_dump[5]);
      }
      break;
    case 5:
      if (bpf_skb_load_bytes(skb, offset, tail_dump, 5) == 0) {
        TUTU_LOG("skb[%d]=0x%02x", offset + 0, tail_dump[0]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 1, tail_dump[1]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 2, tail_dump[2]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 3, tail_dump[3]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 4, tail_dump[4]);
      }
      break;
    case 4:
      if (bpf_skb_load_bytes(skb, offset, tail_dump, 4) == 0) {
        TUTU_LOG("skb[%d]=0x%02x", offset + 0, tail_dump[0]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 1, tail_dump[1]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 2, tail_dump[2]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 3, tail_dump[3]);
      }
      break;
    case 3:
      if (bpf_skb_load_bytes(skb, offset, tail_dump, 3) == 0) {
        TUTU_LOG("skb[%d]=0x%02x", offset + 0, tail_dump[0]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 1, tail_dump[1]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 2, tail_dump[2]);
      }
      break;
    case 2:
      if (bpf_skb_load_bytes(skb, offset, tail_dump, 2) == 0) {
        TUTU_LOG("skb[%d]=0x%02x", offset + 0, tail_dump[0]);
        TUTU_LOG("skb[%d]=0x%02x", offset + 1, tail_dump[1]);
      }
      break;
    case 1:
      if (bpf_skb_load_bytes(skb, offset, tail_dump, 1) == 0) {
        TUTU_LOG("skb[%d]=0x%02x", offset + 0, tail_dump[0]);
      }
      break;
    }
  }
#endif
}

static __always_inline int is_eth_header_present(struct __sk_buff *skb) {
  void *data     = (void *) (long) skb->data;
  void *data_end = (void *) (long) skb->data_end;

  // 检查数据包长度能否包含以太网头
  if (data + ETH_HLEN <= data_end) {
    struct ethhdr *eth = data;
    // 再次确认协议类型
    __be16 eth_proto = eth->h_proto;

    // EtherType和skb->protocol一致，认为有以太网头
    if (eth_proto == skb->protocol) {
      return 1; // 有以太网头
    } else {
      TUTU_LOG("eth_proto %u skb->protocol: %u", eth_proto, skb->protocol);
    }
  } else {
    TUTU_LOG("packet too small: %u", data_end - data);
  }

  // 其他情况认为没有以太网头
  return 0;
}

// 从icmp头部生成检验和，跳过了checksum本身（视为0)
static __always_inline __wsum icmphdr_cksum(struct icmphdr *icmp) {
  // 计算ICMP头部校验和
  __wsum  sum;
  __be16 *p = (__be16 *) icmp;

  sum = p[0];
  // 跳过checksum
  sum += p[2];
  sum += p[3];

  return sum;
}

// 从icmp检验和恢复udp负载的检验和, 支持icmpv6
static __always_inline __wsum recover_payload_csum_from_icmp(struct icmphdr *icmp, struct ipv6hdr *ipv6, __u32 payload_len) {
  __wsum payload_sum = csum_unfold(~icmp->checksum);
  __wsum icmphdr_sum = icmphdr_cksum(icmp);

  // 将icmp检验和取反后，减去掉icmp头部检验和
  payload_sum = csum_sub(payload_sum, icmphdr_sum);

  if (ipv6) {
    // IPV6下，icmp也要计算icmp伪头部:
    // 由于icmp检验和 = ~(icmp伪头部 + icmp头部 + icmp负载)
    // 所以icmp负载 = ~icmp检验和 - icmp头部 - icmp伪头部
    // 于是还需要减去IPv6 icmp伪头部
    __wsum csum = icmpv6_pseudoheader_sum(ipv6, payload_len + sizeof(struct icmphdr));
    payload_sum = csum_sub(payload_sum, csum);
  }

  return payload_sum;
}

// 从UDP校验和中恢复载荷校验和部分
// UDP总校验和 = ~( pseudo_sum + udp_hdr_sum + payload_sum )
// 所以 payload_sum = ~udp->check - pseudo_sum - udp_hdr_sum
static __always_inline __wsum recovery_payload_csum_from_udp(struct udphdr *udp, __wsum pseudo_sum) {
  __wsum udp_sum = ~udp->check & 0xFFFF;
  // TUTU_LOG("udp sum: 0x%04x", bpf_ntohs(udp_sum));

  __wsum udp_hdr_sum = udp_header_sum(udp);
  // TUTU_LOG("udp header sum: 0x%04x", bpf_ntohs(udp_hdr_sum));

  __wsum payload_sum = csum_sub(udp_sum, pseudo_sum);
  payload_sum        = csum_sub(payload_sum, udp_hdr_sum);
  // TUTU_LOG("payload sum: 0x%04x", bpf_ntohs(payload_sum));

  return payload_sum;
}

// 更新icmp检验和
static __always_inline void update_icmp_cksum(struct iphdr *ipv4, struct ipv6hdr *ipv6, struct icmphdr *icmp,
                                              struct udphdr *udp) {
  __wsum udp_pseudo_sum = 0;

  if (ipv4) {
    udp_pseudo_sum = udp_pseudoheader_sum(ipv4, udp);
  } else if (ipv6) {
    udp_pseudo_sum = udpv6_pseudoheader_sum(ipv6, udp);
  }

  // 计算ICMP头部校验和
  __wsum csum        = icmphdr_cksum(icmp);
  __wsum payload_sum = recovery_payload_csum_from_udp(udp, udp_pseudo_sum);

  // 加上载荷的校验和
  csum = csum_add(csum, payload_sum);

  if (ipv6) {
    // IPV6下，icmp也要计算icmp伪头部:
    // icmp检验和 = ~(icmp伪头部 + icmp头部 + icmp负载)
    __u16  udp_payload_len   = bpf_ntohs(udp->len) - sizeof(*udp);
    __wsum icmp6_pseudo_csum = icmpv6_pseudoheader_sum(ipv6, udp_payload_len + sizeof(struct icmphdr));
    csum                     = csum_add(csum, icmp6_pseudo_csum);
  }
  // 取反得到最终ICMP校验和
  icmp->checksum = csum_fold(csum);
}

// 检查并删除过期会话
static __always_inline int check_age(struct config *cfg, struct session_key *lookup_key, struct session_value *value_ptr) {
  // 检查下age
  __u64 age = value_ptr->age;
  __u64 now = (bpf_ktime_get_ns() / NS_PER_SEC); // 当前时间（秒)

  if (!age || now - age >= cfg->session_max_age) {
    // 太老，需要跳过并删除这个key
    TUTU_LOG("session_map entry: age %lu too old: now is %lu", age, now);
    bpf_map_delete_elem(&session_map, lookup_key);
    return -1;
  }

  // 此时需要更新下会话的寿命，否则过了update_interval会话就消失
  // 可以过1秒才更新，避免大量包造成过大压力
  if (now - age >= 1) {
    int err;

    value_ptr->age = now;
    err            = bpf_map_update_elem(&session_map, lookup_key, value_ptr, BPF_EXIST);
    (void) err;
    TUTU_LOG("session updated: age: %lu: %d", now, err);
  }
  return 0;
}

static __always_inline int _parse_headers(void *data, void *data_end, bool has_eth, __u32 *ip_type, __u32 *l2_len,
                                          __u32 *ip_hdr_len, __u8 *ip_proto, __u32 *ip_proto_offset, __u32 *hdr_len) {
  __u32          local_l2_len = 0;
  struct ethhdr *eth;

  if (has_eth) {
    if (data + sizeof(*eth) > data_end)
      return -1;

    eth          = data;
    local_l2_len = sizeof(*eth);

    switch (eth->h_proto) {
    case bpf_htons(ETH_P_IP):
      goto handle_ipv4;
    case bpf_htons(ETH_P_IPV6):
      goto handle_ipv6;
    default:
      return -1;
    }
  } else {
    struct iphdr *iph_tmp = data;
    if ((void *) (iph_tmp + 1) > data_end)
      return -1;

    switch (iph_tmp->version) {
    case 4:
      goto handle_ipv4;
    case 6:
      goto handle_ipv6;
    default:
      return -1;
    }
  }

handle_ipv4: {
  struct iphdr *ipv4 = data + local_l2_len;
  if ((void *) (ipv4 + 1) > data_end)
    return -1;

  __u32 local_ip_hdr_len = ipv4->ihl * 4;
  if (local_ip_hdr_len < sizeof(*ipv4) || data + local_l2_len + local_ip_hdr_len > data_end)
    return -1;

  *ip_type         = 4;
  *ip_proto        = ipv4->protocol;
  *ip_hdr_len      = local_ip_hdr_len;
  *l2_len          = local_l2_len;
  *hdr_len         = local_l2_len + local_ip_hdr_len;
  *ip_proto_offset = local_l2_len + offsetof(struct iphdr, protocol);
  return 0;
}

handle_ipv6: {
  struct ipv6hdr *ipv6 = data + local_l2_len;
  if ((void *) (ipv6 + 1) > data_end)
    return -1;

  __u8  next_hdr          = ipv6->nexthdr;
  __u32 proto_offset      = local_l2_len + offsetof(struct ipv6hdr, nexthdr);
  __u32 current_hdr_start = local_l2_len + sizeof(*ipv6);

#pragma unroll
  for (int i = 0; i < 8; i++) {
    if (!ipv6_is_ext(next_hdr))
      break;

    struct ipv6_opt_hdr *opt = data + current_hdr_start;
    if ((void *) (opt + 1) > data_end)
      return -1;

    proto_offset = current_hdr_start + offsetof(struct ipv6_opt_hdr, nexthdr);
    current_hdr_start += (opt->hdrlen + 1) << 3;
    if (data + current_hdr_start > data_end)
      return -1;

    next_hdr = opt->nexthdr;
  }

  if (data + current_hdr_start > data_end)
    return -1;

  *ip_type         = 6;
  *ip_proto        = next_hdr;
  *ip_hdr_len      = current_hdr_start - local_l2_len;
  *l2_len          = local_l2_len;
  *hdr_len         = current_hdr_start;
  *ip_proto_offset = proto_offset;
  return 0;
}
}

int link_type = LINK_AUTODETECT;

/*
 * BPF 内联函数，用于解析 L2/L3/L4 头部
 *
 * 功能:
 *  - 解析 L2 (Ethernet, 可选) 和 L3 (IPv4/IPv6) 头部。
 *  - 确定 L4 协议类型和 L3/L4 头部的总长度。
 *  - 计算指向 L4 协议的字段 (protocol/nexthdr) 的精确偏移量。
 *
 * 参数:
 *  - skb: 指向 skb 上下文的指针。
 *  - ip_type: [输出] ip协议类型: 4或者6
 *  - l2_len: [输出] L2 头部长度。
 *  - ip_len: [输出] L3 头部长度 (IPv4 或 IPv6 + 扩展头)。
 *  - ip_proto: [输出] L4 协议的值 (例如 IPPROTO_UDP)。
 *  - ip_proto_offset: [输出] 指向L4协议的字段(ip.protocol或最后一个ipv6.nexthdr)的偏移量，从数据包起始计算。
 *  - hdr_len: [输出] 从数据包开始到 L4 头部起始位置的总偏移量 (l2_len + ip_len)。
 *
 * 返回值:
 *  - 0: 成功。
 *  - -1: 失败（包太短、未知协议等）。
 */
static __always_inline int parse_headers(struct __sk_buff *skb, __u32 *ip_type, __u32 *l2_len, __u32 *ip_len, __u8 *ip_proto,
                                         __u32 *ip_proto_offset, __u32 *hdr_len) {
  void *data, *data_end;
  bool  has_eth;

  *ip_type = *l2_len = *ip_len = *ip_proto_offset = *hdr_len = *ip_proto = 0;

  // 1. 判断是否存在以太网头
  switch (link_type) {
  case LINK_AUTODETECT:
    has_eth = is_eth_header_present(skb);
    break;
  case LINK_ETH:
    has_eth = true;
    break;
  case LINK_NONE:
  default:
    has_eth = false;
    break;
  }

  // TUTU_LOG("has_eth: %u, link_type: %u", has_eth, link_type);

  // ingress/egress也可能没有pull完整
  // 首先取出ipv4头部
  {
    int needed_len = (has_eth ? sizeof(struct ethhdr) : 0) + sizeof(struct iphdr);
    int err;

    needed_len = needed_len > skb->len ? skb->len : needed_len;
    err        = bpf_skb_pull_data(skb, needed_len);
    if (err)
      return err;
    data     = (void *) (long) skb->data;
    data_end = (void *) (long) skb->data_end;
  }

  return _parse_headers(data, data_end, has_eth, ip_type, l2_len, ip_len, ip_proto, ip_proto_offset, hdr_len);
}

static __always_inline int update_ipv4_checksum(struct __sk_buff *skb, struct iphdr *ipv4, __u32 l2_len, __u32 old_proto,
                                                __u32 new_proto) {
  const __be16 old_word     = bpf_htons((ipv4->ttl << 8) | old_proto);
  const __be16 new_word     = bpf_htons((ipv4->ttl << 8) | new_proto);
  const int    check_offset = l2_len + offsetof(struct iphdr, check);
  // 用 bpf_l3_csum_replace 增量修正 IP 校验和
  // 注意 offset 要对应 IP check 字段在 skb->data 的偏移
  return bpf_l3_csum_replace(skb, check_offset, old_word, new_word, sizeof(new_word));
}

// Outgoing packet handler (UDP -> ICMP)
SEC("tc/egress")
int handle_egress(struct __sk_buff *skb) {
  int   err;
  __u32 ip_end = 0, ip_proto_offset = 0, l2_len, ip_len, ip_type;
  __u8  ip_proto;
  // 更新统计
  struct config *cfg;

  {
    __u32 key = 0;
    cfg       = try2_p_ok(bpf_map_lookup_elem(&config_map, &key), "config_map cannot be found");
  }

  try_ok(parse_headers(skb, &ip_type, &l2_len, &ip_len, &ip_proto, &ip_proto_offset, &ip_end));
  try_ok(ip_proto == IPPROTO_UDP ? 0 : -1);
  // 重新pull 整个以太网+ip头部+udp头部
  // 不需要整个udp包：因为udp负载没有被修改过，也不需要检查udp负载长度
  try_ok(bpf_skb_pull_data(skb, ip_end + sizeof(struct udphdr)));

  decl_ok(struct udphdr, udp, ip_end, skb);

  // 非法的udp长度
  if (bpf_ntohs(udp->len) < sizeof(*udp)) {
    err = TC_ACT_OK;
    goto err_cleanup;
  }

  struct iphdr   *ipv4 = NULL;
  struct ipv6hdr *ipv6 = NULL;

  if (ip_type == 4) {
    redecl_ok(struct iphdr, ipv4, l2_len, skb);
  } else if (ip_type == 6) {
    redecl_ok(struct ipv6hdr, ipv6, l2_len, skb);
  }

  struct user_info *user = NULL;
  __be16            icmp_id, icmp_seq;
  __u8              icmp_type = 0, uid;

#ifdef BPF_DEBUG
  if (ipv4) {
    // UDP payload length
    __be32 saddr = get_unaligned(&ipv4->saddr);
    __be32 daddr = get_unaligned(&ipv4->daddr);

    __u16 udp_payload_len = bpf_ntohs(udp->len) - sizeof(*udp);
    (void) saddr;
    (void) daddr;
    (void) udp_payload_len;
    TUTU_LOG("Outgoing UDP: %08x:%5d -> %08x:%5d, length: %u", bpf_ntohl(saddr), bpf_ntohs(udp->source), bpf_ntohl(daddr),
             bpf_ntohs(udp->dest), udp_payload_len);
  } else if (ipv6) {
    // UDP payload length
    // TODO
    struct in6_addr saddr;
    struct in6_addr daddr;

    ipv6_copy(&saddr, &ipv6->saddr);
    ipv6_copy(&daddr, &ipv6->daddr);
    __u16 udp_payload_len = bpf_ntohs(udp->len) - sizeof(*udp);
    (void) saddr;
    (void) daddr;
    (void) udp_payload_len;
    TUTU_LOG("Outgoing UDP: %016llx%016llx:%5u -> %016llx%016llx:%5u, length: %u",
             bpf_be64_to_cpu(*(__be64 *) &saddr.s6_addr[0]), bpf_be64_to_cpu(*(__be64 *) &saddr.s6_addr[8]),
             bpf_ntohs(udp->source), bpf_be64_to_cpu(*(__be64 *) &daddr.s6_addr[0]),
             bpf_be64_to_cpu(*(__be64 *) &daddr.s6_addr[8]), bpf_ntohs(udp->dest), udp_payload_len);
  }
#endif

  if (cfg->is_server) {
    // Server mode: Find user by destination IP and source port
    struct session_key lookup_key = {
      .dport = udp->source, // 目的端口为udp源端口
      .sport = udp->dest,   // 源端口为udp目的端口
    };

    if (ipv4) {
      struct in6_addr in6;
      ipv4_to_ipv6_mapped(get_unaligned(&ipv4->daddr), &in6);
      lookup_key.address = in6;
    } else if (ipv6) {
      ipv6_copy(&lookup_key.address, &ipv6->daddr);
    }

    // TUTU_LOG("search port: %5u, sport: %5u", bpf_ntohs(lookup_key.port), bpf_ntohs(lookup_key.sport));
    struct session_value *value_ptr = bpf_map_lookup_elem(&session_map, &lookup_key);
    if (!value_ptr) {
      if (ipv4) {
        TUTU_LOG("cannot get uid: 0x%08X:%5u -> 0x%08X:%5u", bpf_ntohl(get_unaligned(&ipv4->saddr)), bpf_ntohs(udp->source),
                 bpf_ntohl(get_unaligned(&ipv4->daddr)), bpf_ntohs(udp->dest));
      } else if (ipv6) {
        (void) get_unaligned64;
        TUTU_LOG("cannot get uid: 0x%016llx%016llx:%5u -> 0x%016llx%016llx:%5u",
                 bpf_be64_to_cpu(get_unaligned64(&ipv6->saddr.s6_addr[0])),
                 bpf_be64_to_cpu(get_unaligned64(&ipv6->saddr.s6_addr[8])), bpf_ntohs(udp->source),
                 bpf_be64_to_cpu(get_unaligned64(&ipv6->daddr.s6_addr[0])),
                 bpf_be64_to_cpu(get_unaligned64(&ipv6->daddr.s6_addr[8])), bpf_ntohs(udp->dest));
      }
      return TC_ACT_OK;
    }

    uid = value_ptr->uid;
    try2_ok(check_age(cfg, &lookup_key, value_ptr), "check age: %d", _ret);
    user = try2_p_ok(bpf_map_lookup_elem(&user_map, &uid), "invalid uid: %u", uid);

    if (ipv4) {
      icmp_type = ICMP_ECHO_REPLY;
    } else if (ipv6) {
      icmp_type = ICMP6_ECHO_REPLY;
    }

    icmp_id  = user->icmp_id;
    icmp_seq = value_ptr->client_sport;
  } else {
    struct egress_peer_key peer_key = {
      .port = udp->dest,
    };

    if (ipv4) {
      TUTU_LOG("egress: udp: 0x%08x:%5u", bpf_ntohl(get_unaligned(&ipv4->saddr)), udp->dest);
      ipv4_to_ipv6_mapped(get_unaligned(&ipv4->daddr), &peer_key.address);
    } else if (ipv6) {
      TUTU_LOG("egress: udp: src 0x%016llx%016llx:%5u", bpf_be64_to_cpu(get_unaligned64(&ipv6->saddr.s6_addr[0])),
               bpf_be64_to_cpu(get_unaligned64(&ipv6->saddr.s6_addr[8])), udp->dest);
      ipv6_copy(&peer_key.address, &ipv6->daddr);
    }

    struct egress_peer_value *peer_value = try2_p_ok(bpf_map_lookup_elem(&egress_peer_map, &peer_key),
                                                     "egress client: unrelated packet");

    {
      __u32 gso_segs = skb->gso_segs;
      if (gso_segs > 1) {
        TUTU_LOG("egress warning: cannot handle GSO Packets(%u): length: %u", gso_segs, skb->len);
        TUTU_LOG("  Please disable it with QUIC_GO_DISABLE_GSO=1");
        __sync_fetch_and_add(&cfg->gso, 1);
        return TC_ACT_SHOT;
      }
    }

    if (ipv4) {
      // 如果UDP包为分片（包括第一个包或后续包），无法重写后续包没有的UDP头部，直接丢包
      if ((bpf_ntohs(ipv4->frag_off) & 0x1FFF) != 0 || (bpf_ntohs(ipv4->frag_off) & 0x2000) != 0) {
        // 检查分片偏移和MF标志，只要有分片相关标志就丢弃
        TUTU_LOG("egress warning: drop fragmented UDP packet");
        __sync_fetch_and_add(&cfg->fragmented, 1);
        return TC_ACT_SHOT;
      }
    }

    uid = peer_value->uid;

    if (ipv4) {
      icmp_type = ICMP_ECHO_REQUEST;
    } else if (ipv6) {
      icmp_type = ICMP6_ECHO_REQUEST;
    }

    // icmp_id也使用源端口, 服务器有可能看到被nat修改后的新值
    icmp_id = icmp_seq = udp->source;
  }

  __sync_fetch_and_add(&cfg->packets_processed, 1);

#if 0
  {
    TUTU_LOG("dumping original packet:");
    dump_skb(skb);
    TUTU_LOG("====================END====================");
  }
#endif

  struct udphdr old_udp = *udp;

  _Static_assert(sizeof(struct icmphdr) == sizeof(struct udphdr), "ICMP and UDP header sizes must match");

  // Create an ICMP header in place of the UDP header
  struct icmphdr icmp_hdr = {
    .type     = icmp_type,
    .code     = uid,
    .un       = {.echo = {.id = icmp_id, .sequence = icmp_seq}},
    .checksum = 0,
  };

  // TUTU_LOG("sizeof(old_udp): %u", sizeof(old_udp));
  // TUTU_LOG("sizeof(icmp_hdr): %u", sizeof(icmp_hdr));

  // debug_hexdump_8("old_udp", &old_udp);
  // debug_hexdump_8("icmp_hdr", &icmp_hdr);
  (void) bpf_volatile_memcmp;
  (void) bpf_memcmp;
  (void) debug_hexdump_8;
  (void) debug_hexdump_4;
  (void) dump_skb;

  if (!old_udp.check) {
    TUTU_LOG("udp must has checksum!");
    __sync_fetch_and_add(&cfg->packets_dropped, 1);
    return TC_ACT_SHOT;
  }

  if (cfg->no_fixup) {
    update_icmp_cksum(ipv4, ipv6, &icmp_hdr, udp);
  }

  // TUTU_LOG("icmp hdr checksum: 0x%04x", bpf_ntohs(icmp_hdr.checksum);

  // 将UDP头部替换为ICMP头部
  err = bpf_skb_store_bytes(skb, ip_end, &icmp_hdr, sizeof(icmp_hdr), 0);
  if (err) {
    __sync_fetch_and_add(&cfg->packets_dropped, 1);
    TUTU_LOG("bpf_skb_store_bytes failed: %d", err);
    return TC_ACT_SHOT;
  }

  // 写入字节之后， ipv4 & ipv6不再能访问
  // 需要重设指针
  if (ipv4) {
    redecl_ok(struct iphdr, ipv4, l2_len, skb);
  } else if (ipv6) {
    redecl_ok(struct ipv6hdr, ipv6, l2_len, skb);
  }

  // 修改IP协议为ICMP
  // 只有ipv4才需要修复ip头部检验和
  __u8 new_proto = IPPROTO_ICMPV6;
  if (ipv4) {
    new_proto = IPPROTO_ICMP;

    err = update_ipv4_checksum(skb, ipv4, l2_len, IPPROTO_UDP, new_proto);
    if (err) {
      __sync_fetch_and_add(&cfg->packets_dropped, 1);
      TUTU_LOG("update_ipv4_checksum failed: %d", err);
      return TC_ACT_SHOT;
    }
  }
  err = bpf_skb_store_bytes(skb, ip_proto_offset, &new_proto, sizeof(new_proto), 0);

  if (err) {
    __sync_fetch_and_add(&cfg->packets_dropped, 1);
    TUTU_LOG("bpf_skb_store_bytes failed: %d", err);
    return TC_ACT_SHOT;
  }

  if (!cfg->no_fixup) {
    // 后门函数:
    // ipv4: 如果是硬件offload：设置csum_offset为icmphdr->checksum位置。不需要修改csum_start，继续硬件offload
    // ipv4: 如果是软件计算: 重新算整个icmp的检验和，停止硬件计算
    // ipv6: 重新算整个icmpv6的检验和，停止硬件计算
    bpf_skb_change_type(skb, MAGIC_FLAG3);
  }

  {
#ifdef BPF_DEBUG
    __u16 udp_payload_len = bpf_ntohs(old_udp.len) - sizeof(*udp);
    (void) udp_payload_len;
#endif
    TUTU_LOG("  Rebuilt ICMP: id: %u, seq: %u, type: %u, code: %u, length: %u", bpf_ntohs(icmp_hdr.un.echo.id),
             bpf_ntohs(icmp_hdr.un.echo.sequence), icmp_type, uid, udp_payload_len);
    // dump_skb(skb);
  }

  err = TC_ACT_OK;
err_cleanup:
  return err;
}

static __always_inline int update_session_map(struct user_info *user, __u8 uid, __be16 icmp_seq) {
  int                   err;
  struct session_key    key   = {.dport = user->dport, .sport = user->icmp_id, .address = user->address};
  struct session_value *exist = bpf_map_lookup_elem(&session_map, &key);
  __u64                 now   = (bpf_ktime_get_ns() / NS_PER_SEC); // 当前时间（秒）

  if (exist) {
    bool client_sport_changed = exist->client_sport != icmp_seq;
    bool uid_changed          = exist->uid != uid;
    bool age_exceed_1s        = (now > exist->age) && (now - exist->age > 1);

    // 若没有变化且未超过 1 秒，不更新
    if (!client_sport_changed && !uid_changed && !age_exceed_1s) {
      return 0;
    }
  }

  struct session_value value = {
    .uid          = uid,
    .age          = now,
    .client_sport = icmp_seq,
  };

  err = bpf_map_update_elem(&session_map, &key, &value, BPF_ANY);
  TUTU_LOG("update session_map: sport %5u, dport: %5u, age: %5u, client_sport: %5u, ret: %d", bpf_ntohs(key.sport),
           bpf_ntohs(key.dport), value.age, bpf_ntohs(value.client_sport), err);
  return err;
}

// 更新udp检验和
static __always_inline void update_udp_cksum(struct iphdr *ipv4, struct ipv6hdr *ipv6, struct udphdr *udp, __wsum payload_sum) {
  __wsum total_sum, pseudo_sum = 0;

  // 先把 UDP头部的检验和和负载的检验和加在一起
  total_sum = csum_add(udp_header_sum(udp), payload_sum);

  if (ipv4) {
    // 加上 IPv4 伪头部
    pseudo_sum = udp_pseudoheader_sum(ipv4, udp);
  } else if (ipv6) {
    // 加上 IPv6 伪头部
    pseudo_sum = udpv6_pseudoheader_sum(ipv6, udp);
  } else {
    TUTU_LOG("neither ipv4 nor ipv6 packet");
  }

  // 折叠
  udp->check = csum_fold(csum_add(total_sum, pseudo_sum));

  // rfc768规定
  if (!udp->check)
    udp->check = CSUM_MANGLED_0;
}

#ifdef ENABLE_XDP_INGRESS
static __always_inline int parse_headers_xdp(void *data, void *data_end, __u32 *ip_type, __u32 *l2_len, __u32 *ip_len,
                                             __u8 *ip_proto, __u32 *ip_proto_offset, __u32 *hdr_len) {
  *ip_type = *l2_len = *ip_len = *ip_proto_offset = *hdr_len = *ip_proto = 0;
  return _parse_headers(data, data_end, true, ip_type, l2_len, ip_len, ip_proto, ip_proto_offset, hdr_len);
}

static __always_inline int update_ipv4_checksum_xdp(struct iphdr *iph, __u8 old_proto, __u8 new_proto) {
  const __be16 old_word = bpf_htons((iph->ttl << 8) | old_proto);
  const __be16 new_word = bpf_htons((iph->ttl << 8) | new_proto);

  __wsum csum = csum_unfold(~iph->check);
  csum        = csum_sub(csum, csum_unfold(old_word));
  csum        = csum_add(csum, csum_unfold(new_word));
  iph->check  = csum_fold(csum);

  return 0;
}
#endif

static __always_inline int parse_headers_ingress(void *ctx, __u32 *ip_type, __u32 *l2_len, __u32 *ip_len, __u8 *ip_proto,
                                                 __u32 *ip_proto_offset, __u32 *ip_end) {
#ifdef ENABLE_XDP_INGRESS
  void *data     = (void *) (long) ((struct xdp_md *) ctx)->data;
  void *data_end = (void *) (long) ((struct xdp_md *) ctx)->data_end;
  int   err;

  err = parse_headers_xdp(data, data_end, ip_type, l2_len, ip_len, ip_proto, ip_proto_offset, ip_end);
  if (err)
    return err;

  if (data + *ip_end + sizeof(struct icmphdr) > data_end)
    return -1;

  return 0;
#else
  return parse_headers((struct __sk_buff *) ctx, ip_type, l2_len, ip_len, ip_proto, ip_proto_offset, ip_end);
#endif
}

// xdp分支返回XDP_PASS，而不是TC_ACT_OK
#ifdef ENABLE_XDP_INGRESS
#undef try_ok
#undef try2_p_ok
#undef try2_ok
#undef decl_ok
#undef redecl_ok
#define try_ok    try_pass
#define try2_p_ok try2_p_pass
#define try2_ok   try2_pass
#define decl_ok   decl_pass
#define redecl_ok redecl_pass
#endif

static inline int xmit_ingress_ok(void) {
  return
#ifdef ENABLE_XDP_INGRESS
    XDP_PASS
#else
    TC_ACT_OK
#endif
    ;
}

// Incoming packet handler (ICMP -> UDP)
#ifdef ENABLE_XDP_INGRESS
SEC("xdp.frags")
#else
SEC("tc/ingress")
#endif
int handle_ingress(
#ifdef ENABLE_XDP_INGRESS
  struct xdp_md *ctx
#else
  struct __sk_buff *ctx
#endif

) {
  int            err;
  __u32          ip_end = 0, ip_proto_offset = 0, l2_len, ip_len, ip_type;
  __u8           ip_proto;
  struct config *cfg;

  (void) xdp_data_end;

  {
    __u32 key = 0;
    cfg       = try2_p_ok(bpf_map_lookup_elem(&config_map, &key));
  }

  try2_ok(parse_headers_ingress(ctx, &ip_type, &l2_len, &ip_len, &ip_proto, &ip_proto_offset, &ip_end));
  try2_ok(ip_proto == IPPROTO_ICMP || ip_proto == IPPROTO_ICMPV6 ? 0 : -1);

#ifndef ENABLE_XDP_INGRESS
  // 重新pull 整个以太网+ip头部+icmp/icmp6头部
  try2_ok(bpf_skb_pull_data(ctx, ip_end + sizeof(struct icmphdr)), "pull data failed: %d", _ret);
#endif
  _Static_assert(sizeof(struct icmphdr) == sizeof(struct icmp6hdr), "ICMP and ICMPv6 header sizes must match");

  struct iphdr   *ipv4 = NULL;
  struct ipv6hdr *ipv6 = NULL;

  if (ip_type == 4) {
    redecl_ok(struct iphdr, ipv4, l2_len, ctx);
  } else if (ip_type == 6) {
    redecl_ok(struct ipv6hdr, ipv6, l2_len, ctx);
  }

  {
    // 至少应该有l3头部长度再加上icmp头部
    u32 least_size = ip_len + sizeof(struct icmphdr);

    // 检查ipv4/ipv6报文长度是否合理
    if (ipv4) {
      // IPv4: icmp报文全长必须足够
      if (bpf_ntohs(ipv4->tot_len) < least_size) {
        err = xmit_ingress_ok();
        goto err_cleanup;
      }
    } else if (ipv6) {
      // 守护
      if (ip_len < sizeof(struct ipv6hdr)) {
        err = xmit_ingress_ok();
        goto err_cleanup;
      }
      // IPV6的payload_len: 不包括基本头部的剩余数据包长度
      // 加上ipv6头部后必须足够
      if (sizeof(struct ipv6hdr) + bpf_ntohs(ipv6->payload_len) < least_size) {
        err = xmit_ingress_ok();
        goto err_cleanup;
      }
    }
  }

  decl_ok(struct icmphdr, icmp, ip_end, ctx);

  // Extract UID from ICMP code
  __u8   uid      = icmp->code;
  __be16 icmp_seq = icmp->un.echo.sequence;
  __be16 icmp_id  = icmp->un.echo.id;

  struct user_info *user = NULL;

  // Check if this is a tunnel packet
  if (cfg->is_server) {
    // Server: Check for ECHO_REQUEST and valid UID
    if (ipv4) {
      try_ok(icmp->type == ICMP_ECHO_REQUEST ? 0 : -1);
    } else if (ipv6) {
      try_ok(icmp->type == ICMP6_ECHO_REQUEST ? 0 : -1);
    }
    // Find user by UID
    user = try2_p_ok(bpf_map_lookup_elem(&user_map, &uid), "cannot get user: %u", uid);

    // 验证客户端地址与用户配置地址相等
    if (ipv4) {
      struct in6_addr in6;
      ipv4_to_ipv6_mapped(get_unaligned(&ipv4->saddr), &in6);
      try2_ok(ipv6_equal(&user->address, &in6) ? 0 : -1, "unrelated client ipv4 address");
    } else if (ipv6) {
      try2_ok(ipv6_equal(&user->address, &ipv6->saddr) ? 0 : -1, "unrelated client ipv6 address");
    }

    // 优化：只有发生变化才需要更新user_map
    // icmp_id：客户端更新了icmp_id，此时需要更新
    if (user->icmp_id != icmp_id) {
      user->icmp_id = icmp_id;
      // Update user info
      bpf_map_update_elem(&user_map, &uid, user, BPF_EXIST);
    }

    // 需要更新session_map
    try2_ok(update_session_map(user, uid, icmp_seq), "update session map: %d", _ret);
  } else {
    if (ipv4) {
      try_ok(icmp->type == ICMP_ECHO_REPLY ? 0 : -1);
    } else if (ipv6) {
      try_ok(icmp->type == ICMP6_ECHO_REPLY ? 0 : -1);
    }
  }

  __be16 udp_src, udp_dst;

  if (cfg->is_server) {
    try2_ok(user ? 0 : -1, "no user?");
    // Server mode
    // 使用icmp_id作为源端口: nat转换后的值,一定是唯一的
    udp_src = user->icmp_id;
    udp_dst = user->dport;
  } else {
    struct ingress_peer_key peer_key = {
      .uid = icmp->code,
    };

    if (ipv4) {
      TUTU_LOG("ingress: icmp: 0x%08x:%u", bpf_ntohl(get_unaligned(&ipv4->saddr)), icmp->code);
      ipv4_to_ipv6_mapped(get_unaligned(&ipv4->saddr), &peer_key.address);
    } else if (ipv6) {
      TUTU_LOG("ingress: icmp: src 0x%016llx%016llx id %u", bpf_be64_to_cpu(get_unaligned64(&ipv6->saddr.s6_addr[0])),
               bpf_be64_to_cpu(get_unaligned64(&ipv6->saddr.s6_addr[8])), icmp->code);
      ipv6_copy(&peer_key.address, &ipv6->saddr);
    }

    struct ingress_peer_value *peer_value = try2_p_ok(bpf_map_lookup_elem(&ingress_peer_map, &peer_key),
                                                      "ingress client: unrelated packet");
    udp_src                               = peer_value->port;
    udp_dst                               = icmp_seq; // Use ICMP sequence as destination port
  }

  __sync_fetch_and_add(&cfg->packets_processed, 1);

#ifndef ENABLE_XDP_INGRESS
  {
    __u32 gso_segs = ctx->gso_segs;
    if (gso_segs > 1) {
      TUTU_LOG("ingress warning: cannot handle GSO Packets(%u): length: %u", gso_segs, ctx->len);
      TUTU_LOG("  Please disable interface GSO");
      __sync_fetch_and_add(&cfg->gso, 1);
      return TC_ACT_SHOT;
    }
  }
#endif

  struct in6_addr saddr, daddr;

  if (ipv4) {
    ipv4_to_ipv6_mapped(get_unaligned(&ipv4->saddr), &saddr);
    ipv4_to_ipv6_mapped(get_unaligned(&ipv4->daddr), &daddr);
  } else if (ipv6) {
    ipv6_copy(&saddr, &ipv6->saddr);
    ipv6_copy(&daddr, &ipv6->daddr);
  }

  // ICMP payload length
  __u16 payload_len = 0;

  if (ipv4) {
    payload_len = bpf_ntohs(ipv4->tot_len) - ip_len - sizeof(struct icmphdr);
  } else if (ipv6) {
    // ip_len需要减去ipv6头部,因为ipv6的长度是负载长度(不包括ipv6头)
    payload_len = bpf_ntohs(ipv6->payload_len) - (ip_len - sizeof(struct ipv6hdr)) - sizeof(struct icmp6hdr);
  }

  struct udphdr udp_hdr = {
    .source = udp_src,
    .dest   = udp_dst,
    .len    = bpf_htons(sizeof(struct udphdr) + payload_len),
    .check  = 0,
  };

  struct icmphdr old_icmp;

#ifdef __mips__
  bpf_probe_read_kernel(&old_icmp, sizeof(old_icmp), icmp);
#else
  old_icmp = *icmp;
#endif

#ifdef BPF_DEBUG
  TUTU_LOG("Incoming ICMP: 0x%016llx%016llx -> 0x%016llx%016llx", bpf_be64_to_cpu(*(__be64 *) &saddr.s6_addr[0]),
           bpf_be64_to_cpu(*(__be64 *) &saddr.s6_addr[8]));
  TUTU_LOG("  id: %u, seq: %u, length: %u, ", bpf_htons(icmp_id), bpf_htons(icmp_seq), payload_len);
#endif

#if 0
  debug_hexdump_8("icmp hdr", &old_icmp);
#endif

#if 0
  debug_hexdump_8("ippart1", ip);
  debug_hexdump_8("ippart2", ((void*)ip)+8);
  debug_hexdump_4("ippart3", ((void*)ip)+8+4);
#endif

  // 只有ipv4才需要修复ip头部检验和
  __u8 new_proto = IPPROTO_UDP;
  if (ipv4) {
#ifdef ENABLE_XDP_INGRESS
    ipv4->protocol = new_proto;
    update_ipv4_checksum_xdp(ipv4, IPPROTO_ICMP, new_proto);
#else
    err = update_ipv4_checksum(ctx, ipv4, l2_len, IPPROTO_ICMP, new_proto);
    if (err) {
      __sync_fetch_and_add(&cfg->packets_dropped, 1);
      TUTU_LOG("update_ipv4_checksum failed: %d", err);
      return TC_ACT_SHOT;
    }

    err = bpf_skb_store_bytes(ctx, ip_proto_offset, &new_proto, sizeof(new_proto), 0);

    if (err) {
      __sync_fetch_and_add(&cfg->packets_dropped, 1);
      TUTU_LOG("bpf_skb_store_bytes failed: %d", err);
      return TC_ACT_SHOT;
    }
#endif
  }

#ifndef ENABLE_XDP_INGRESS
  // TC-BPF: 写入字节之后， ipv4 & ipv6不再能访问
  // 需要重设指针
  if (ipv4) {
    redecl_ok(struct iphdr, ipv4, l2_len, ctx);
  } else if (ipv6) {
    redecl_ok(struct ipv6hdr, ipv6, l2_len, ctx);
  }
#endif

  _Static_assert(sizeof(struct icmphdr) == sizeof(struct udphdr), "ICMP and UDP header sizes must match");
  _Static_assert(sizeof(udp_hdr) == 8, "ICMP and UDP header sizes must match");

  {
    __wsum payload_sum = recover_payload_csum_from_icmp(&old_icmp, ipv6, payload_len);

#if 0
    TUTU_LOG("payload sum: %04x", payload_sum);
#endif

    update_udp_cksum(ipv4, ipv6, &udp_hdr, payload_sum);
  }

#ifdef ENABLE_XDP_INGRESS
#ifdef __mips__
  __builtin_memcpy((void *) (volatile __u8 *) icmp, (const void *) (volatile __u8 *) &udp_hdr, sizeof(udp_hdr));
#else
  __builtin_memcpy(icmp, &udp_hdr, sizeof(udp_hdr));
#endif
#else
  // Replace ICMP header with UDP header
  err = bpf_skb_store_bytes(ctx, ip_end, &udp_hdr, sizeof(udp_hdr), 0);
  if (err) {
    __sync_fetch_and_add(&cfg->packets_dropped, 1);
    TUTU_LOG("bpf_skb_store_bytes failed: %d", err);
    return TC_ACT_SHOT;
  }
#endif

  {
    TUTU_LOG("  Rebuilt UDP: 0x%016llx%016llx:%5u -> 0x%016llx%016llx:%5u, length: %u",
             bpf_be64_to_cpu(*(__be64 *) &saddr.s6_addr[0]), bpf_be64_to_cpu(*(__be64 *) &saddr.s6_addr[8]), bpf_ntohs(udp_src),
             bpf_be64_to_cpu(*(__be64 *) &daddr.s6_addr[0]), bpf_be64_to_cpu(*(__be64 *) &daddr.s6_addr[8]), bpf_ntohs(udp_dst),
             payload_len);
    // dump_skb(skb);
  }

  err = xmit_ingress_ok();
err_cleanup:
  return err;
}

#ifndef DISABLE_BPF_TIMER
static long gc_foreach_cb(struct bpf_map *map, const void *_key, void *_value, void *ctx) {
  const struct session_key *key   = _key;
  struct session_value     *value = _value;
  struct config            *cfg;
  __u32                     key0 = 0;

  cfg = bpf_map_lookup_elem(&config_map, &key0);
  if (!cfg)
    goto out;

  // 检查下age
  __u64 age = value->age;
  __u64 now = (bpf_ktime_get_ns() / NS_PER_SEC); // 当前时间（秒)

  if (!age || now - age >= cfg->session_max_age) {
    // 太老，需要跳过并删除这个key
    TUTU_LOG("gc_foreach_cb: age %u too old: now is %u", age, now);
    bpf_map_delete_elem(&session_map, key);
  }

out:
  return 0;
}

// 注意bpf verifier要求gc_tick永远返回0
static int gc_tick(void *map, __u32 *key, struct gc_timer *gc) {
  int               err;
  struct gc_switch *gc_switch = NULL;
  __u32             key0      = 0;

  TUTU_LOG("gc_tick called");

  gc_switch = bpf_map_lookup_elem(&gc_switch_map, &key0);
  if (!gc_switch) {
    TUTU_LOG("cannot find gc_switch_map");
    goto out;
  }

  // 如果 gc 被禁用了，则不执行清理，也不重新调度
  if (!gc_switch->enabled) {
    TUTU_LOG("gc is disabled, stopping timer.");
    goto out;
  }

  err = bpf_for_each_map_elem(&session_map, &gc_foreach_cb, NULL, 0);
  if (err < 0) {
    TUTU_LOG("bpf_for_each_map_elem failed with %d", err);
  }

  gc = bpf_map_lookup_elem(&gc_timer_map, &key0);
  if (!gc) {
    TUTU_LOG("cannot find gc_timer_map");
    goto out;
  }

  err = bpf_timer_start(&gc->timer, NS_PER_SEC, 0);
  if (err) {
    TUTU_LOG("bpf_timer_start failed with %d", err);
  }

out:
  return 0;
}

// 本函数需要tuctl(bpf_prog_test_run_opts)发送一个至少14字节的假skb
// 切换服务器模式时需要调用这个bpf:
// 如果定时器没有初始化，就初始化定时器
// 然后启动定时器。此时如果gc_switch->enabled为false将不执行任何任务，也不继续激活timer
SEC("tc")
int handle_gc_timer(struct __sk_buff *skb) {
  (void) skb;

  int              err  = 0;
  struct gc_timer *gc   = NULL;
  __u32            key0 = 0;

  TUTU_LOG("gc_timer");
  // 如果没有初始化gc_timer_map的0号元素就插入一个
  gc = bpf_map_lookup_elem(&gc_timer_map, &key0);
  if (!gc) {
    struct gc_timer zero = {};

    bpf_map_update_elem(&gc_timer_map, &key0, &zero, BPF_NOEXIST);
    gc = try2_p_shot(bpf_map_lookup_elem(&gc_timer_map, &key0), "init gc_timer map failed");
  }

  // 初始化定时器，如果已经存在就跳过callback初始化(-EBUSY)
  err = bpf_timer_init(&gc->timer, &gc_timer_map, CLOCK_MONOTONIC);
  if (err && err != -EBUSY) {
    TUTU_LOG("bpf timer init: %d", err);
    return TC_ACT_SHOT;
  }

  if (err != -EBUSY) {
    try2_shot(bpf_timer_set_callback(&gc->timer, gc_tick), "bpf timer set callback: %d", _ret);
  }

  // 启动定时器，如果gc_switch->enabled为假不执行任何任务，也不继续激活timer
  try2_shot(bpf_timer_start(&gc->timer, NS_PER_SEC, 0), "bpf_timer_start failed with %d", _ret);

  err = 0;
err_cleanup:
  return err;
}
#endif

char _license[] SEC("license") = "GPL";

// vim: set sw=2 ts=2 expandtab:
