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
  struct sk_buff                    *skb;
  bool                               fixed = false;

  if (!params || !params->skb || params->type != MAGIC_FLAG3)
    return 0;

  pr_info_once("magic called\n");
  skb = params->skb;

  if (skb->protocol == htons(ETH_P_IP)) {
    struct iphdr *iph       = ip_hdr(skb);
    unsigned int  l4_offset = skb_network_offset(skb) + iph->ihl * 4;
    int           err;

    // ICMP
    if (iph->protocol == IPPROTO_ICMP) {
      struct icmphdr *icmph;
      size_t          ip_hdr_len = (size_t) (iph->ihl * 4);
      size_t          icmp_len, writable_len;
      bool            use_partial;

      if (ntohs(iph->tot_len) < ip_hdr_len)
        return 0;
      if (l4_offset > skb->len)
        return 0;
      icmp_len = ntohs(iph->tot_len) - ip_hdr_len;
      icmp_len = min_t(size_t, icmp_len, skb->len - l4_offset);
      if (icmp_len < sizeof(*icmph))
        return 0;

      use_partial = !force_sw_checksum && skb->ip_summed == CHECKSUM_PARTIAL &&
                    skb_checksum_start_offset(skb) == (int) l4_offset;

      writable_len = l4_offset + (use_partial ? sizeof(*icmph) : icmp_len);
      err          = skb_ensure_writable(skb, writable_len);
      if (unlikely(err))
        return 0;

      icmph = (struct icmphdr *) (skb->data + l4_offset);

      if (use_partial) {
        /*
         * CHECKSUM_PARTIAL 下的 ICMPv4：
         * ICMPv4 校验和只覆盖 ICMP 头和数据，不包含 IPv4 pseudo-header，
         * 因此这里把 checksum 字段清零即可，硬件从 csum_start 算到包尾后
         * 写回的就是最终校验和。
         */
        icmph->checksum  = 0;
        skb->csum_offset = offsetof(struct icmphdr, checksum);
      } else {
        icmph->checksum = 0;
        icmph->checksum = csum_fold(csum_partial((char *) icmph, (int) icmp_len, 0));
        skb->ip_summed  = CHECKSUM_UNNECESSARY;
      }
      fixed = true;
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
      struct icmp6hdr *icmp6h;
      size_t           ext_hdr_len;
      size_t           icmp_len, writable_len;
      bool             use_partial;

      if (l4_offset < skb_network_offset(skb) + sizeof(struct ipv6hdr))
        return 0;
      ext_hdr_len = l4_offset - skb_network_offset(skb) - sizeof(struct ipv6hdr);
      if (ntohs(ip6h->payload_len) < ext_hdr_len)
        return 0;
      icmp_len = ntohs(ip6h->payload_len) - ext_hdr_len;
      icmp_len = min_t(size_t, icmp_len, skb->len - l4_offset);
      if (icmp_len < sizeof(*icmp6h))
        return 0;

      use_partial = !force_sw_checksum && skb->ip_summed == CHECKSUM_PARTIAL &&
                    skb_checksum_start_offset(skb) == (int) l4_offset;

      writable_len = l4_offset + (use_partial ? sizeof(*icmp6h) : icmp_len);
      err          = skb_ensure_writable(skb, writable_len);
      if (unlikely(err))
        return 0;

      ip6h   = ipv6_hdr(skb);
      icmp6h = (struct icmp6hdr *) (skb->data + l4_offset);

      if (use_partial) {
        /*
         * CHECKSUM_PARTIAL 下的 ICMPv6：
         * 硬件通常只计算从 csum_start 到包尾的校验和，但 ICMPv6 校验和
         * 还必须包含 IPv6 pseudo-header。因此这里不能清零，而要先写入
         * pseudo-header 的补偿值，这样硬件再累加 ICMPv6 头和数据后，
         * 得到的才是最终校验和。
         */
        icmp6h->icmp6_cksum = ~csum_ipv6_magic(&ip6h->saddr, &ip6h->daddr, icmp_len, IPPROTO_ICMPV6, 0);
        skb->csum_offset    = offsetof(struct icmp6hdr, icmp6_cksum);
      } else {
        __wsum csum;

        icmp6h->icmp6_cksum = 0;
        csum                = csum_partial((char *) icmp6h, (int) icmp_len, 0);
        icmp6h->icmp6_cksum = csum_ipv6_magic(&ip6h->saddr, &ip6h->daddr, icmp_len, IPPROTO_ICMPV6, csum);
        skb->ip_summed      = CHECKSUM_UNNECESSARY;
      }
      fixed = true;
    }
  }

  if (fixed)
    regs_set_return_value(regs, 0);
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
