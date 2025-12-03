#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/icmp.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/kprobes.h>
#include <linux/printk.h>
#include <linux/ptrace.h>
#include <linux/stddef.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <net/checksum.h>
#include <net/ip6_checksum.h>

#include "csum-hack.h"

#ifdef __mips__
static inline void regs_set_return_value(struct pt_regs *regs, unsigned long rc) {
  regs->regs[2] = rc;
}
#endif

struct bpf_skb_change_type_params {
  struct sk_buff *skb;
  u32             type;
};

static int bpf_skb_change_type_entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs) {
  struct bpf_skb_change_type_params *params = (typeof(params)) ri->data;
#if 0
  for (int i=0; i<32; i++) {
    pr_err("regs->regs[%d]: 0x%08lx\n", i, regs->regs[i]);
  }
#endif
#if defined(__arm__)
  params->skb  = (void *) regs->uregs[0];
  params->type = regs->uregs[1];
#elif defined(__mips__) // o32/n32/n64
  params->skb  = (void *) regs->regs[4];
  params->type = regs->regs[6];
#else
  params->skb  = (void *) regs_get_kernel_argument(regs, 0);
  params->type = regs_get_kernel_argument(regs, 1);
#endif
  return 0;
}
NOKPROBE_SYMBOL(bpf_skb_change_type_entry_handler);

static int force_sw_checksum = 0;
module_param(force_sw_checksum, int, 0644);
MODULE_PARM_DESC(force_sw_checksum, "Force software checksum calculation for all ICMP packets");

static int bpf_skb_change_type_ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs) {
  unsigned long retval = regs_return_value(regs);

  if (retval != -EINVAL)
    return 0;

  struct bpf_skb_change_type_params *params = (typeof(params)) ri->data;
  struct sk_buff                    *skb    = params->skb;

  if (!params->skb || params->type != MAGIC_FLAG3)
    return 0;
  pr_info_once("magic called\n");
  if (skb->protocol == htons(ETH_P_IP)) {
    struct iphdr *iph       = ip_hdr(skb);
    unsigned int  l4_offset = skb_network_offset(skb) + iph->ihl * 4;
    int           err;

    // ICMP
    if (iph->protocol == IPPROTO_ICMP) {
      if (force_sw_checksum || skb->ip_summed == CHECKSUM_NONE) {
        // 本来如果没有使用硬件计算检验和bpf可以直接处理不需要调用本后门函数，但为了兼容错误配置起见还是替bpf修复检验和
        iph                        = ip_hdr(skb);
        struct icmphdr *icmph      = (typeof(icmph)) ((char *) iph + iph->ihl * 4);
        size_t          ip_hdr_len = (size_t) (iph->ihl * 4);
        size_t          icmp_len   = ntohs(iph->tot_len) - ip_hdr_len;

        icmp_len = min_t(size_t, icmp_len, skb->len - l4_offset);

        err = skb_ensure_writable(skb, l4_offset + icmp_len); // 整个icmp包
        if (unlikely(err))
          return 0;

        iph             = ip_hdr(skb);
        icmph           = (typeof(icmph)) ((char *) iph + ip_hdr_len);
        icmph->checksum = 0;
        icmph->checksum = csum_fold(csum_partial((char *) icmph, (int) icmp_len, 0));
        skb->ip_summed  = CHECKSUM_UNNECESSARY;
      } else if (skb->ip_summed == CHECKSUM_PARTIAL) {
        // skb->csum_start: 不变，因为udp->icmp并没有修改包长度
        // skb->csum_offset: 应该指向icmp头部检验和位置
        skb->csum_offset = offsetof(struct icmphdr, checksum);
      }
    }
  } else if (skb->protocol == htons(ETH_P_IPV6)) {
    struct ipv6hdr *ip6h      = ipv6_hdr(skb);
    unsigned int    l4_offset = skb_network_offset(skb) + sizeof(struct ipv6hdr);
    __u8            nexthdr   = ip6h->nexthdr;
    int             err;

    // 处理扩展头部
    if (ipv6_ext_hdr(nexthdr)) {
      __be16 frag_off;
      int    offset = ipv6_skip_exthdr(skb, sizeof(struct ipv6hdr), &nexthdr, &frag_off);
      if (offset < 0)
        return 0;
      l4_offset = skb_network_offset(skb) + offset;
    }

    // ICMPv6
    if (nexthdr == IPPROTO_ICMPV6) {
      struct icmp6hdr *icmp6h      = (typeof(icmp6h)) (skb->data + l4_offset);
      unsigned int     ext_hdr_len = l4_offset - skb_network_offset(skb) - sizeof(struct ipv6hdr);
      unsigned int     icmp_len    = ntohs(ip6h->payload_len) - ext_hdr_len;

      // print_hex_dump_bytes("icmp6h: ", DUMP_PREFIX_ADDRESS, icmp6h, sizeof(*icmp6h));
      icmp_len = min_t(size_t, icmp_len, skb->len - l4_offset);

      if (force_sw_checksum || skb->ip_summed == CHECKSUM_NONE) {
        err = skb_ensure_writable(skb, l4_offset + icmp_len); // 整个icmp包
        if (unlikely(err))
          return 0;

        ip6h                = ipv6_hdr(skb);
        icmp6h              = (typeof(icmp6h)) (skb->data + l4_offset);
        icmp6h->icmp6_cksum = 0;
        // 同样为了兼容性修复icmpv6检验和
        __wsum csum = csum_partial((char *) icmp6h, (int) icmp_len, 0);
        // 计算 ICMPv6 校验和(带伪头部)
        icmp6h->icmp6_cksum = csum_ipv6_magic(&ip6h->saddr, &ip6h->daddr, icmp_len, IPPROTO_ICMPV6, csum);
        skb->ip_summed      = CHECKSUM_UNNECESSARY;
      } else if (skb->ip_summed == CHECKSUM_PARTIAL) {
        // 先保证 L4 头可写
        err = skb_ensure_writable(skb, l4_offset + sizeof(struct icmp6hdr)); // UDP/ICMPv6头至少8字节
        if (unlikely(err))
          return 0;

        ip6h   = ipv6_hdr(skb);
        icmp6h = (struct icmp6hdr *) (skb->data + l4_offset);
        // 计算 ICMPv6 校验和: 只算icmpv6伪头部，让硬件完成整个检验和计算
        // 由于csum_ipv6_magic()结果是最终检验和，需要反转才得到icmpv6伪头部
        icmp6h->icmp6_cksum = ~csum_ipv6_magic(&ip6h->saddr, &ip6h->daddr, icmp_len, IPPROTO_ICMPV6, 0);
        // 而skb->csum_offset应该指向icmpv6头部检验和位置
        skb->csum_offset = offsetof(struct icmp6hdr, icmp6_cksum);
      }
    }
  }

  regs_set_return_value(regs, params->skb->ip_summed);
  return 0;
}
NOKPROBE_SYMBOL(bpf_skb_change_type_ret_handler);

static struct kretprobe bpf_skb_change_type_probe = {
  .kp.symbol_name = "bpf_skb_change_type",
  .entry_handler  = bpf_skb_change_type_entry_handler,
  .handler        = bpf_skb_change_type_ret_handler,
  .data_size      = sizeof(struct bpf_skb_change_type_params),
  .maxactive      = 32,
};

static struct kretprobe *mimic_probes[] = {
  &bpf_skb_change_type_probe,
};

static int csum_hack_init(void) {
  return register_kretprobes(mimic_probes, ARRAY_SIZE(mimic_probes));
}

static void csum_hack_exit(void) {
  unregister_kretprobes(mimic_probes, ARRAY_SIZE(mimic_probes));
}

MODULE_DESCRIPTION("eBPF ICMP obfuscator - kernel module extension");
MODULE_LICENSE("GPL");

static int __init mimic_init(void) {
  int ret = csum_hack_init();
  return ret;
}

static void __exit mimic_exit(void) {
  csum_hack_exit();
}

module_init(mimic_init);
module_exit(mimic_exit);

// vim: set sw=2 ts=2 expandtab:
