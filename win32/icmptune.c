#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "network.h"

#include "log.h"
#include "parser.h"
#include "resolve.h"
#include "try.h"
#include "windivert.h"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef signed char        s8;
typedef signed short       s16;
typedef signed int         s32;
typedef unsigned long long u64;
typedef signed long long   s64;

typedef u16 __be16;

#include "net_proto.h"

typedef struct {
  u8  type;
  u8  code;
  u16 checksum;
  u16 id;
  u16 seq;
} icmp_hdr;

uint16_t   server_port;
uint8_t    local_uid;
extern int log_verbosity;

HANDLE send_handle;
HANDLE recv_handle;

/******************************************************************************/

// 地址字符串化
int addr_to_str(const struct sockaddr_storage *addr, char *out, size_t len, int *ipv4) {
  const struct sockaddr *sa = (const struct sockaddr *) addr;

  *ipv4 = 1;
  if (sa->sa_family == AF_INET) {
    // 纯 IPv4，直接转换
    const struct sockaddr_in *sin4 = (const struct sockaddr_in *) addr;
    return inet_ntop(AF_INET, &sin4->sin_addr, out, len) ? 0 : -errno;
  }

  if (sa->sa_family == AF_INET6) {
    const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *) addr;

    // 如果是 IPv4-mapped IPv6，转换为 IPv4 字符串
    if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
      struct in_addr a4;
      memcpy(&a4, &sin6->sin6_addr.s6_addr[12], 4);
      return inet_ntop(AF_INET, &a4, out, len) ? 0 : -errno;
    }

    *ipv4 = 0;
    // 其他情况：输出标准 IPv6 文本表示
    return inet_ntop(AF_INET6, &sin6->sin6_addr, out, (socklen_t) len) ? 0 : -errno;
  }

  // 其他协议族也视为不支持
  log_error("Unsupported address family: %d", sa->sa_family);
  return -ENOSYS;
}

void hex_dump(char *str, const void *buf, int size) {
  int            i, j;
  const uint8_t *ubuf = buf;

  if (log_verbosity < LOG_DEBUG)
    return;

  if (str)
    printf("%s:\n", str);

  for (i = 0; i < size; i += 16) {
    printf("%4x: ", i);
    for (j = 0; j < 16; j++) {
      if ((i + j) < size) {
        printf(" %02x", ubuf[j]);
      } else {
        printf("   ");
      }
    }
    printf("  ");
    for (j = 0; j < 16 && (i + j) < size; j++) {
      if (ubuf[j] >= 0x20 && ubuf[j] <= 0x7f) {
        printf("%c", ubuf[j]);
      } else {
        printf(".");
      }
    }
    printf("\n");
    ubuf += 16;
  }
  printf("\n");
}

/******************************************************************************/
int send_loop(void) {
  uint8_t packet[2048];

  while (1) {
    PWINDIVERT_IPHDR     ip       = NULL;
    PWINDIVERT_IPV6HDR   ipv6     = NULL;
    PWINDIVERT_ICMPHDR   icmp4    = NULL;
    PWINDIVERT_ICMPV6HDR icmp6    = NULL;
    PWINDIVERT_TCPHDR    tcp      = NULL;
    PWINDIVERT_UDPHDR    udp      = NULL;
    UINT8                protocol = 0;
    PVOID                data     = NULL;
    UINT                 dataLen  = 0;
    PVOID                next     = NULL;
    UINT                 nextLen  = 0;

    WINDIVERT_ADDRESS addr;
    UINT32            packet_len = 0;

    int retv = WinDivertRecv(send_handle, packet, sizeof(packet), &packet_len, &addr);
    if (retv == 0) {
      printf("Failed to read packet! (%ld)\n", GetLastError());
      continue;
    }

    if (!WinDivertHelperParsePacket(packet, packet_len, &ip, &ipv6, &protocol, &icmp4, &icmp6, &tcp, &udp, &data, &dataLen,
                                    &next, &nextLen))
      continue;

    if (udp == NULL) {
      // 不是 UDP 包，跳过
      continue;
    }

    // 复用 UDP 头所在位置当 ICMP 头使用
    icmp_hdr *icmp = (icmp_hdr *) udp;

    __be16 src_port = udp->SrcPort;

    if (ip != NULL) {
      ip->Checksum = 0;
      // IPv4: UDP -> ICMPv4 Echo Request
      ip->Protocol = IPPROTO_ICMP;
      icmp->type   = ICMP_ECHO_REQUEST;
    } else if (ipv6 != NULL) {
      // IPv6: UDP -> ICMPv6 Echo Request
      ipv6->NextHdr = IPPROTO_ICMPV6;
      icmp->type    = ICMP6_ECHO_REQUEST; // Echo Request
    } else {
      // 既不是 IPv4 也不是 IPv6
      continue;
    }

    icmp->code     = local_uid;
    icmp->checksum = 0;
    icmp->id       = src_port;
    icmp->seq      = src_port;

    // 让 WinDivert 自动重算 IPv4/IPv6 + ICMPv4/ICMPv6 校验和（IPv6 会自动处理伪首部）
    WinDivertHelperCalcChecksums(packet, packet_len, &addr, 0);

    hex_dump("sending", packet, packet_len);

    retv = WinDivertSend(send_handle, packet, packet_len, NULL, &addr);
    if (retv == 0) {
      printf("Failed to send packet! (%ld)\n", GetLastError());
      continue;
    }
  }
}

DWORD recv_thread(void *arg) {
  uint8_t packet[2048];
  (void) arg;

  while (1) {
    PWINDIVERT_IPHDR     ip       = NULL;
    PWINDIVERT_IPV6HDR   ipv6     = NULL;
    PWINDIVERT_ICMPHDR   icmp4    = NULL;
    PWINDIVERT_ICMPV6HDR icmp6    = NULL;
    PWINDIVERT_TCPHDR    tcp      = NULL;
    PWINDIVERT_UDPHDR    udp      = NULL;
    UINT8                protocol = 0;
    PVOID                data     = NULL;
    UINT                 dataLen  = 0;
    PVOID                next     = NULL;
    UINT                 nextLen  = 0;

    WINDIVERT_ADDRESS addr;
    UINT32            packet_len = 0;

    int retv = WinDivertRecv(recv_handle, packet, sizeof(packet), &packet_len, &addr);
    if (retv == 0) {
      printf("Failed to read packet! (%ld)\n", GetLastError());
      continue;
    }

    if (!WinDivertHelperParsePacket(packet, packet_len, &ip, &ipv6, &protocol, &icmp4, &icmp6, &tcp, &udp, &data, &dataLen,
                                    &next, &nextLen))
      continue;

    // 应该是 ICMPv4 或 ICMPv6
    icmp_hdr *icmp = NULL;
    if (icmp4 != NULL) {
      icmp = (icmp_hdr *) icmp4;
    } else if (icmp6 != NULL) {
      icmp = (icmp_hdr *) icmp6;
    } else {
      continue; // 不是 ICMP，跳过
    }

    // 防止越界：至少要有一个完整的 ICMP Echo 头
    UINT l4_offset = (UINT) ((uint8_t *) icmp - packet);
    if (l4_offset + sizeof(icmp_hdr) > packet_len) {
      continue;
    }

    // 从 seq 里取出原来的 UDP 目标端口
    UINT16 dst_port = icmp->seq;

    if (packet_len < l4_offset + sizeof(WINDIVERT_UDPHDR)) {
      // 包太短，不够一个 UDP 头
      continue;
    }
    // 复用 ICMP 头所在位置当 UDP 头
    udp = (PWINDIVERT_UDPHDR) icmp;

    if (ip != NULL) {
      ip->Protocol = IPPROTO_UDP;
      ip->Checksum = 0;
    } else if (ipv6 != NULL) {
      ipv6->NextHdr = IPPROTO_UDP;
    } else {
      continue;
    }

    // data / dataLen 是 L4 payload（原 ICMP 数据），我们没有改大小
    // 所以 UDP 长度 = UDP 头 + payload
    udp->SrcPort  = htons(server_port);
    udp->DstPort  = dst_port;
    udp->Length   = htons((UINT) (sizeof(WINDIVERT_UDPHDR) + dataLen));
    udp->Checksum = 0;

    WinDivertHelperCalcChecksums(packet, packet_len, &addr, 0);

    retv = WinDivertSend(recv_handle, packet, packet_len, NULL, &addr);
    if (retv == 0) {
      printf("Failed to inject packet! (%ld)\n", GetLastError());
      continue;
    }
  }
}

/******************************************************************************/

int main(int argc, char *argv[]) {
  char                filter[256];
  int                 err, opt, family = AF_UNSPEC, ipv4 = 0;
  struct sockaddr_in6 ip6 = {
    .sin6_family = AF_INET6,
  };
  char server_ip[128];

  while ((opt = getopt(argc, argv, "46d")) != -1) {
    switch (opt) {
    case '4':
      family = AF_INET;
      break;
    case '6':
      family = AF_INET6;
      break;
    case 'd':
      log_verbosity = LOG_DEBUG;
      log_debug("debug enabled");
      break;
    default:
      goto usage;
      break;
    }
  }

  if (argc - optind < 3) {
  usage:
    printf("Usage: icmptune  [-4|-6|-d] uid server_domain_or_ip server_port\n");
    return -EINVAL;
  }

  WSADATA wsa;
  try2(WSAStartup(MAKEWORD(2, 2), &wsa), "WSAStartup failed: %d", _ret);

  try2(parse_uid(argv[optind], &local_uid));
  try2(resolve_ip_addr(family, argv[optind + 1], &ip6.sin6_addr));
  try2(addr_to_str((struct sockaddr_storage *) &ip6, server_ip, sizeof(server_ip), &ipv4));
  try2(parse_port(argv[optind + 2], &server_port));
  log_info("uid: %u, server_ip: %s, dport: %u", local_uid, server_ip, server_port);

  if (argc - optind > 3) {
    log_warn("Warning: extra arguments ignored");
  }

  log_debug("family = %s", family == AF_INET ? "AF_INET" : family == AF_INET6 ? "AF_INET6" : "AF_UNSPEC");

  //////////////// Client Recv ////////////////
  if (ipv4) {
    snprintf(filter, sizeof(filter), "ip.SrcAddr == %s and icmp.Type == 0 and icmp.Code == 0x%02x", server_ip, local_uid);
  } else {
    snprintf(filter, sizeof(filter), "ipv6.SrcAddr == %s and icmpv6.Type == %d and icmpv6.Code == 0x%02x", server_ip,
             ICMP6_ECHO_REPLY, local_uid);
  }

  filter[sizeof(filter) - 1] = '\0';

  recv_handle = WinDivertOpen(filter, WINDIVERT_LAYER_NETWORK, 0, 0);
  if (recv_handle == INVALID_HANDLE_VALUE) {
    log_error("Failed to open the WinDivert device for recv! (%ld)", GetLastError());
    err = -1;
    goto err_cleanup;
  }
  log_info("recv filter: %s", filter);

  WinDivertSetParam(recv_handle, WINDIVERT_PARAM_QUEUE_LENGTH, WINDIVERT_PARAM_QUEUE_LENGTH_MAX);
  WinDivertSetParam(recv_handle, WINDIVERT_PARAM_QUEUE_TIME, WINDIVERT_PARAM_QUEUE_TIME_MAX);
  WinDivertSetParam(recv_handle, WINDIVERT_PARAM_QUEUE_SIZE, WINDIVERT_PARAM_QUEUE_SIZE_MAX);

  HANDLE thread = CreateThread(NULL, 1, (LPTHREAD_START_ROUTINE) recv_thread, NULL, 0, NULL);
  if (!thread) {
    log_error("Create thread failed!(%ld)", GetLastError());
    err = -1;
    goto err_cleanup;
  }

  //////////////// Client Send ////////////////
  if (ipv4) {
    snprintf(filter, sizeof(filter), "ip.DstAddr == %s and udp.DstPort == %d", server_ip, server_port);
  } else {
    snprintf(filter, sizeof(filter), "ipv6.DstAddr == %s and udp.DstPort == %d", server_ip, server_port);
  }
  filter[sizeof(filter) - 1] = '\0';

  send_handle = WinDivertOpen(filter, WINDIVERT_LAYER_NETWORK, 0, 0);
  if (send_handle == INVALID_HANDLE_VALUE) {
    log_error("Failed to open the WinDivert device for send! (%ld)", GetLastError());
    err = -1;
    goto err_cleanup;
  }
  log_info("send filter: %s", filter);

  WinDivertSetParam(send_handle, WINDIVERT_PARAM_QUEUE_LENGTH, WINDIVERT_PARAM_QUEUE_LENGTH_MAX);
  WinDivertSetParam(send_handle, WINDIVERT_PARAM_QUEUE_TIME, WINDIVERT_PARAM_QUEUE_TIME_MAX);
  WinDivertSetParam(send_handle, WINDIVERT_PARAM_QUEUE_SIZE, WINDIVERT_PARAM_QUEUE_SIZE_MAX);

  send_loop();
  err = 0;
err_cleanup:
  WSACleanup();
  return err;
}

// vim: set sw=2 ts=2 expandtab:
