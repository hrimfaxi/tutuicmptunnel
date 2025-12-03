#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../tucrypto/tucrypto.h"
#include "common.h"
#include "log.h"
#include "read_script.h"
#include "resolve.h"
#include "tuparser.h"

#include "try.h"

#define DEFAULT_SERVER "127.0.0.1"

typedef struct {
  char    *script;
  char    *psk;
  char    *server;
  int      server_port;
  int      family;
  uint32_t window;
  uint16_t max_retries;
} args_t;

static int print_client_usage(int argc, char **argv) {
  (void) argc;
  fprintf(stderr,
          "Usage: %s psk PSK [script SCRIPT|-] [ server SERVER] [server-port SERVER_PORT] "
          "[window WINDOW] [max-retries MAX_RETRIES] [-4] [-6]\n",
          argv[0]);
  return 0;
}

static int parse_dsl_args(int argc, char **argv, args_t *out) {
  int         err          = -EINVAL;
  const char *psk          = NULL;
  const char *server       = DEFAULT_SERVER;
  uint16_t    server_port  = DEFAULT_SERVER_PORT;
  const char *script       = "-";
  int         family       = AF_UNSPEC;
  size_t      content_size = 0;
  uint16_t    max_retries  = 3;
  uint32_t    window       = DEFAULT_WINDOW;
  char       *psk_dup = NULL, *server_dup = NULL, *content = NULL;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (matches(tok, "server-port")) {
      if (++i >= argc)
        goto usage;

      try(parse_port(argv[i], &server_port));
    } else if (matches(tok, "script")) {
      if (++i >= argc)
        goto usage;

      script = argv[i];
    } else if (matches(tok, "psk")) {
      if (++i >= argc)
        goto usage;

      psk = argv[i];
    } else if (matches(tok, "server")) {
      if (++i >= argc)
        goto usage;

      server = argv[i];
    } else if (matches(tok, "window")) {
      if (++i >= argc)
        goto usage;

      try(parse_window(argv[i], &window));
    } else if (matches(tok, "max-retries")) {
      if (++i >= argc)
        goto usage;

      try(parse_u16(argv[i], &max_retries));
    } else if (matches(tok, "-4")) {
      family = AF_INET;
    } else if (matches(tok, "-6")) {
      family = AF_INET6;
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      /* 未知关键字 */
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_client_usage(argc, argv), -EINVAL;
    }
  }

  if (!psk) {
    log_error("psk must be specified");
    goto usage;
  }

  if (strlen(psk) < 8) {
    log_error("PSK must be at least 8 characters long");
    goto usage;
  }

  if (!script)
    script = "-";

  try2(read_script(script, &content, &content_size), "read_script");
  try2(strdup_safe(psk, &psk_dup), "strdup");
  try2(strdup_safe(server, &server_dup), "strdup");

  out->script      = content;
  out->psk         = psk_dup;
  out->server      = server_dup;
  out->server_port = server_port;
  out->window      = window;
  out->family      = family;
  out->max_retries = max_retries;
  err              = 0;

err_cleanup:
  if (err) {
    free(content);
    free(psk_dup);
    free(server_dup);
  }
  return err;
}

static void free_args(args_t *a) {
  free(a->script);
  free(a->psk);
  free(a->server);
}

static int set_sock_timeout(int sock, int timeout_sec) {
  const char *blob     = NULL;
  size_t      blob_len = 0;

#ifdef _WIN32
  DWORD timeout_ms = (DWORD) timeout_sec * 1000;

  blob     = (const char *) &timeout_ms;
  blob_len = sizeof(timeout_ms);
#else
  struct timeval tv = {
    .tv_sec  = timeout_sec,
    .tv_usec = 0,
  };

  blob     = (const char *) &tv;
  blob_len = sizeof(tv);
#endif

  return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, blob, blob_len);
}

int main(int argc, char **argv) {
  int     err = 0, sock = -1;
  args_t  a          = {};
  size_t  packet_len = 0;
  char    cmd[MAX_PT_SIZE];
  uint8_t pt[MAX_PT_SIZE];
  int     timeout    = 5;
  int     replay_max = DEFAULT_REPLAY_MAX;

  struct replay_window rwin;
  int                  rwin_inited = 0;

#ifdef _WIN32
  WSADATA wsa;
  try2(WSAStartup(MAKEWORD(2, 2), &wsa), "WSAStartup failed: %d", _ret);
#endif
  setup_pwhash_memlimit();

#ifdef USE_SODIUM
  try2(sodium_init(), "sodium initialize failed: %d", _ret);
#endif
  try2(parse_dsl_args(argc, argv, &a));

  replay_window_init(&rwin, a.window, replay_max);
  rwin_inited = 1;

  uint16_t retries = 0;
retry:;
  uint8_t pad[256];
  size_t  pad_len = tucrypto_randombytes_uniform(256);
  memset(pad, '#', pad_len);

  size_t cmd_len = (size_t) try2(scnprintf(cmd, sizeof(cmd), "%s", a.script), "scnprintf: %d", _ret);
  if (cmd_len + pad_len > sizeof(cmd) - 1) {
    pad_len = sizeof(cmd) - 1 - cmd_len;
  }
  memcpy(cmd + cmd_len, pad, pad_len);
  cmd_len += pad_len;
  cmd[cmd_len] = '\0';

  sock = try2(socket(AF_INET6, SOCK_DGRAM, 0), "socket: %s", strret);

#ifdef _WIN32
  int off = 0; // 0 = 允许 IPv4-mapped (dual-stack)
  setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char *) &off, sizeof(off));
#endif

  struct sockaddr_in6 dst = {
    .sin6_family = AF_INET6,
    .sin6_port   = htons(a.server_port),
  };

  try2(resolve_ip_addr(a.family, a.server, &dst.sin6_addr), "resolve_ip_addr: %s", strret);
  try2(encrypt_and_send_packet(sock, (const struct sockaddr *) &dst, sizeof(dst), &rwin, a.psk, (char *) cmd, cmd_len,
                               &packet_len),
       "encrypt_and_send_packet: %s", strret);
  log_info("sent %zu bytes to %s:%d", packet_len, a.server, a.server_port);
  try2(set_sock_timeout(sock, timeout), "set_sock_timeout: %s", strret);

  uint8_t                 buf[MAX_CT_SIZE];
  struct sockaddr_storage cli;
  socklen_t               clen = sizeof(cli);
  ssize_t                 len  = recvfrom(sock, (void *) buf, sizeof(buf), 0, (struct sockaddr *) &cli, &clen);

  if (len < 0) {
#ifdef _WIN32
    if (WSAGetLastError() == WSAETIMEDOUT)
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK)
#endif
    {
      // 超时
      log_error("recvfrom timeout");
      if (retries++ < a.max_retries) {
        sleep_ms(100);
        log_info("performing retries: %u / %u", retries, a.max_retries);
        close(sock);
        sock = -1;
        goto retry;
      }
    } else {
      // 其他错误
#ifdef _WIN32
      log_error("recvfrom WSA error=%d", WSAGetLastError());
#else
      log_error("recvfrom error: %s", strerror(errno));
#endif
    }

    err = -1;
    goto err_cleanup;
  }

  if (len < (ssize_t) MIN_LEN) {
    char abuf[128];
    addr_to_str(&cli, abuf, sizeof(abuf));
    log_error("drop: short packet from %s", abuf);
    err = -EINVAL;
    goto err_cleanup;
  }

  unsigned long long pt_len = 0;
  try2(decrypt_and_validate_packet(pt, &pt_len, buf, len, &rwin, a.psk, &cli), "decrypt_and_validate_packet: %s", strret);

  {
    char abuf[128];

    try2(addr_to_str(&cli, abuf, sizeof(abuf)));
    try2(remove_padding(pt, &pt_len));
    log_info("response from %s: %.*s", abuf, (int) pt_len, pt);
  }

err_cleanup:
  if (sock >= 0)
    close(sock);
  free_args(&a);
  if (rwin_inited)
    replay_window_free(&rwin);
#ifdef _WIN32
  WSACleanup();
#endif
  return err;
}

// vim: set sw=2 ts=2 expandtab:
