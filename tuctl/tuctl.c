#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <ctype.h>
#include <getopt.h>
#include <linux/limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <time.h>

#include "defs.h"
#include "list.h"
#include "log.h"
#include "parser.h"
#include "resolve.h"
#include "try.h"
#include "tuctl.h"

#include "tutuicmptunnel.debug.skel.h"
#include "tutuicmptunnel.skel.h"

#ifndef LIBBPF_VERSION_GEQ
#if defined(LIBBPF_MAJOR_VERSION) && defined(LIBBPF_MINOR_VERSION)
#define LIBBPF_VERSION_GEQ(major, minor)                                                                                       \
  (LIBBPF_MAJOR_VERSION > (major) || (LIBBPF_MAJOR_VERSION == (major) && LIBBPF_MINOR_VERSION >= (minor)))
#else
#define LIBBPF_VERSION_GEQ(major, minor) (0)
#endif
#endif

/*
 * BPF_MAP_FOREACH(fd, key_type, val_type, key_var, val_var, body)
 * --------------------------------------------------------------
 * 迭代 map 中所有元素。body 内部可以使用 key_var 和 val_var，
 * 如果要删除当前元素，直接 bpf_map_delete_elem(fd, &key_var) 即可。
 */
#define BPF_MAP_FOREACH(fd, key_t, val_t, key, val, body)                                                                      \
  do {                                                                                                                         \
    key_t key = {0}, __next_key = {0};                                                                                         \
    val_t val   = {0};                                                                                                         \
    int   __err = bpf_map_get_next_key((fd), NULL, &key);                                                                      \
    while (!__err) {                                                                                                           \
      /* 提前算好 next_key，删除当前元素也安全 */                                                                              \
      __err = bpf_map_get_next_key((fd), &key, &__next_key);                                                                   \
      if (!bpf_map_lookup_elem((fd), &key, &val)) {                                                                            \
        body;                                                                                                                  \
      }                                                                                                                        \
      key = __next_key;                                                                                                        \
    }                                                                                                                          \
  } while (0)

#define BUILD_TYPE_MAP_PATH   PIN_DIR "build_type_map"
#define CONFIG_MAP_PATH       PIN_DIR "config_map"
#define EGRESS_PEER_MAP_PATH  PIN_DIR "egress_peer_map"
#define INGRESS_PEER_MAP_PATH PIN_DIR "ingress_peer_map"
#define USER_MAP_PATH         PIN_DIR "user_map"
#define SESSION_MAP_PATH      PIN_DIR "session_map"
#define GC_SWITCH_MAP_PATH    PIN_DIR "gc_switch_map"
#define HANDLE_EGRESS_PATH    PIN_DIR "handle_egress"
#define HANDLE_INGRESS_PATH   PIN_DIR "handle_ingress"
#define HANDLE_GC_TIMER_PATH  PIN_DIR "handle_gc_timer"

#define TC_HANDLE_DEFAULT   1
#define TC_PRIORITY_DEFAULT 1

typedef struct {
  const char *name;
  int (*handler)(int argc, char **argv);
  const char *desc;
} subcommand_t;

// 子命令处理函数声明
int cmd_load(int argc, char **argv);
int cmd_unload(int argc, char **argv);
int cmd_server(int argc, char **argv);
int cmd_client(int argc, char **argv);
int cmd_client_add(int argc, char **argv);
int cmd_client_del(int argc, char **argv);
int cmd_server_add(int argc, char **argv);
int cmd_server_del(int argc, char **argv);
int cmd_reaper(int argc, char **argv);
int cmd_status(int argc, char **argv);
int cmd_dump(int argc, char **argv);
int cmd_script(int argc, char **argv);
int cmd_help(int argc, char **argv);
int cmd_version(int argc, char **argv);

static int set_build_type_map(int fd, uint32_t build_type) {
  uint32_t key = 0;

  return bpf_map_update_elem(fd, &key, &build_type, 0);
}

static int get_build_type_map(int fd, uint32_t *build_type) {
  uint32_t key = 0;

  return bpf_map_lookup_elem(fd, &key, build_type);
}

static int set_config_map(int fd, const struct config *cfg) {
  uint32_t key = 0;

  return bpf_map_update_elem(fd, &key, cfg, 0);
}

#ifndef DISABLE_BPF_TIMER
static int set_gc_switch_map(int fd, const struct gc_switch *gc) {
  uint32_t key = 0;

  return bpf_map_update_elem(fd, &key, gc, 0);
}
#endif

static int get_config_map(int fd, struct config *cfg) {
  uint32_t key = 0;

  return bpf_map_lookup_elem(fd, &key, cfg);
}

static int set_egress_peer_map(int fd, const struct egress_peer_key *key, const struct egress_peer_value *value) {
  return bpf_map_update_elem(fd, key, value, 0);
}

static int set_ingress_peer_map(int fd, const struct ingress_peer_key *key, const struct ingress_peer_value *value) {
  return bpf_map_update_elem(fd, key, value, 0);
}

static int check_kmod(void) {
  struct stat s;
  return stat("/sys/module/tutu_csum_fixup", &s);
}

static uid_map_t uids;
static int       numeric = 0;
static int       debug   = 0;
static int       family  = AF_UNSPEC;
static int       help    = 0;

static const char *special_chars = "\"\\$'\r\n";

// 使用完后需要释放string
static int uid2string(uint8_t uid, char **string, int dump) {
  const char *host = NULL;
  int         err;
  char       *uid_fmt = dump ? "uid %u" : "UID: %u";

  if (numeric || uid_map_get_host(&uids, uid, &host))
    return asprintf(string, uid_fmt, uid) != -1 ? 0 : -ENOMEM;

  int needs_quoting = !!strpbrk(host, special_chars);

  if (needs_quoting) {
    char *escaped = NULL;
    try(escapestr(host, &escaped));

    const char *user_fmt = dump ? "user \"%s\"" : "User: \"%s\"";
    err                  = asprintf(string, user_fmt, escaped) != -1 ? 0 : -ENOMEM;
    free(escaped);
  } else {
    const char *user_fmt = dump ? "user %s" : "User: %s";
    err                  = asprintf(string, user_fmt, host) != -1 ? 0 : -ENOMEM;
  }

  return err;
}

static int string2uid(const char *string, uint8_t *uid) {
  int err;
  int tmp_uid = 0;

  err = uid_map_get_uid(&uids, string, &tmp_uid);
  if (!err) {
    *uid = (uint8_t) tmp_uid;
    return 0;
  }

  return parse_uid(string, uid);
}

#ifndef DISABLE_BPF_TIMER
static int run_gc_timer(void) {
  enum { ETH_HLEN = 14 };
  uint8_t                  pkt[ETH_HLEN] = {};
  uint32_t                 pkt_len       = sizeof(pkt);
  int                      gc_timer_fd   = -1, err;
  struct bpf_test_run_opts opts          = {
             .sz           = sizeof(struct bpf_test_run_opts),
             .data_in      = pkt,
             .data_size_in = pkt_len,
  };

  gc_timer_fd = try2(bpf_obj_get(HANDLE_GC_TIMER_PATH), _("bpf_obj_get: %s: %s"), HANDLE_GC_TIMER_PATH, strret);
  try2(bpf_prog_test_run_opts(gc_timer_fd, &opts), "bpf_prog_test_run_opts: %s", strret);
  err = 0;

err_cleanup:
  if (gc_timer_fd >= 0)
    close(gc_timer_fd);
  return err;
}

static int gc_switch(bool on) {
  int              gc_switch_map_fd = -1, err;
  struct gc_switch gc_switch        = {
           .enabled = on,
  };

  gc_switch_map_fd = try2(bpf_obj_get(GC_SWITCH_MAP_PATH), _("bpf_obj_get: %s: %s"), GC_SWITCH_MAP_PATH, strret);
  try2(set_gc_switch_map(gc_switch_map_fd, &gc_switch), _("set_gc_switch_map: %s"), strret);
  err = 0;
err_cleanup:
  if (gc_switch_map_fd >= 0)
    close(gc_switch_map_fd);
  return err;
}
#endif

#define CMD_SERVER_SUMMARY "Set up " STR(PROJECT_NAME) " in server mode"

static int print_server_usage(int argc, char *argv[]) {
  (void) argc;

  fprintf(stderr,
          "Usage: %s %s [OPTIONS]\n\n"

          "  " CMD_SERVER_SUMMARY ".\n\n"

          "Options:\n"
          "  %-15s Set the session aging time in seconds.\n"
          "  %-15s Disable kernel checksum fixup. Use when the interface's\n"
          "  %-15s TX checksumming is turned off.\n",
          STR(PROG_NAME), argv[0], "max-age AGE", "no-fixup", "");
  return 0;
}

int cmd_server(int argc, char **argv) {
  uint8_t  no_fixup        = 0;
  uint32_t session_max_age = 60;
  int      config_map_fd   = -1;
  int      err             = 0;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (matches(tok, "no-fixup") == 0) {
      no_fixup = 1;
    } else if (matches(tok, "max-age") == 0) {
      if (++i >= argc)
        goto usage;
      try(parse_age(argv[i], &session_max_age));
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_server_usage(argc, argv), -EINVAL;
    }
  }

  printf("server mode, session max age=%u, no-fixup=%s\n", session_max_age, no_fixup ? "on" : "off");

  if (!no_fixup && check_kmod()) {
    log_warn("Please load the 'tutu_csum_fixup' kernel module (modprobe tutu_csum_fixup) when checksum fixup is enabled.");
  }

  config_map_fd = try2(bpf_obj_get(CONFIG_MAP_PATH), _("bpf_obj_get: %s: %s"), CONFIG_MAP_PATH, strret);

  struct config cfg = {
    .is_server       = 1,
    .no_fixup        = no_fixup,
    .session_max_age = session_max_age,
  };

  try2(set_config_map(config_map_fd, &cfg), _("set_config_map: %s"), strret);
#ifndef DISABLE_BPF_TIMER
  try2(gc_switch(true), _("gc_switch_on(true): %s"), strret);
  try2(run_gc_timer(), _("run_gc_timer"), strret);
#endif

  err = 0;
err_cleanup:
  if (config_map_fd >= 0)
    close(config_map_fd);
  return err;
}

#define CMD_CLIENT_SUMMARY "Set up " STR(PROJECT_NAME) " in client mode"

static int print_client_usage(int argc, char **argv) {
  (void) argc;
  fprintf(stderr,
          "Usage: %s %s [OPTIONS]\n\n"

          "  " CMD_CLIENT_SUMMARY ".\n\n"

          "Options:\n"
          "  %-15s Disable kernel checksum fixup. Use when the interface's\n"
          "  %-15s TX checksumming is turned off.\n",
          STR(PROG_NAME), argv[0], "no-fixup", "");
  return 0;
}

int cmd_client(int argc, char **argv) {
  uint8_t no_fixup      = 0;
  int     config_map_fd = -1;
  int     err           = 0;

  struct config cfg;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (matches(tok, "no-fixup") == 0) {
      no_fixup = 1;
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_client_usage(argc, argv), -EINVAL;
    }
  }

  if (!no_fixup && check_kmod()) {
    log_warn("Please load the 'tutu_csum_fixup' kernel module (modprobe tutu_csum_fixup) when checksum fixup is enabled.");
  }

  printf("client mode, no-fixup: %s\n", no_fixup ? "on" : "off");
#ifndef DISABLE_BPF_TIMER
  try2(gc_switch(false), _("gc_switch(false): %s"), strret);
#endif
  config_map_fd = try2(bpf_obj_get(CONFIG_MAP_PATH), _("bpf_obj_get: %s: %s"), CONFIG_MAP_PATH, strret);

  cfg = (typeof(cfg)) {
    .is_server = 0,
    .no_fixup  = no_fixup,
  };
  try2(set_config_map(config_map_fd, &cfg));
  err = 0;

err_cleanup:
  if (config_map_fd >= 0)
    close(config_map_fd);
  return err;
}

static int get_server_mode(bool *server) {
  struct config cfg           = {};
  int           config_map_fd = -1;
  int           err;

  config_map_fd = try2(bpf_obj_get(CONFIG_MAP_PATH), _("bpf_obj_get: %s: %s"), CONFIG_MAP_PATH, strret);
  try2(get_config_map(config_map_fd, &cfg), _("get_config_map: %s"), strret);

  if (server)
    *server = cfg.is_server;
  err = 0;

err_cleanup:
  if (config_map_fd >= 0)
    close(config_map_fd);
  return err;
}

#define CMD_CLIENT_ADD_SUMMARY "Add a peer to the client configuration"

static int print_client_add_usage(int argc, char **argv) {
  (void) argc;
  fprintf(stderr,
          "Usage: %s %s [OPTIONS] address ADDR port PORT {uid UID | user USERNAME} [comment COMMENT]\n\n"

          "  " CMD_CLIENT_ADD_SUMMARY ".\n\n"

          "Arguments:\n"
          "  %-22s The peer address (domain name or IP).\n"
          "  %-22s The peer destination port.\n"
          "  %-22s Specify the client by numeric user ID.\n"
          "  %-22s Specify the client by username.\n"
          "  %-22s An optional descriptive comment.\n\n"

          "Options:\n"
          "  %-22s Force domain name resolution to IPv4.\n"
          "  %-22s Force domain name resolution to IPv6.\n"
          "  %-22s Display UID as a number instead of resolving it to a username"
          " in command output. \n",

          STR(PROG_NAME), argv[0], "address ADDR", "port PORT", "uid UID", "user USERNAME", "comment COMMENT", "-4", "-6",
          "-n");
  return 0;
}

int cmd_client_add(int argc, char **argv) {
  const char *address            = NULL;
  const char *comment            = NULL;
  uint16_t    port               = 0;
  uint8_t     uid                = 0;
  bool        uid_set            = false;
  int         egress_peer_map_fd = -1, ingress_peer_map_fd = -1;
  int         err       = 0;
  bool        is_server = false;

  struct egress_peer_key    egress_peer_key;
  struct egress_peer_value  egress_peer_value;
  struct ingress_peer_key   ingress_peer_key;
  struct ingress_peer_value ingress_peer_value;
  struct in6_addr           in6;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (is_address_kw(tok)) {
      if (++i >= argc)
        goto usage;
      address = argv[i];
    } else if (matches(tok, "comment") == 0) {
      if (++i >= argc)
        goto usage;
      comment = argv[i];
    } else if (matches(tok, "port") == 0) {
      if (++i >= argc)
        goto usage;
      try(parse_port(argv[i], &port));
    } else if (is_user_kw(tok)) {
      if (++i >= argc)
        goto usage;
      try(string2uid(argv[i], &uid));
      uid_set = true;
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_client_add_usage(argc, argv), -EINVAL;
    }
  }

  try2(get_server_mode(&is_server));
  if (is_server) {
    log_error("You must be in client mode to add peers.");
    err = -EINVAL;
    goto err_cleanup;
  }

  if (!address || !port || !uid_set) {
    log_error("UID, address, and port must be specified.");
    goto usage;
  }

  egress_peer_key = (typeof(egress_peer_key)) {
    .port = htons(port),
  };
  egress_peer_value = (typeof(egress_peer_value)) {
    .uid = uid,
  };

  try2(resolve_ip_addr(family, address, &in6));
  egress_peer_key.address = in6;

  if (comment) {
    if (strpbrk(comment, "\r\n")) {
      log_error("Multi-line comment is not allowed.");
      err = -EINVAL;
      goto err_cleanup;
    }

    strncpy((char *) egress_peer_value.comment, comment, sizeof(egress_peer_value.comment));
    egress_peer_value.comment[sizeof(egress_peer_value.comment) - 1] = '\0';
  }

  egress_peer_map_fd = try2(bpf_obj_get(EGRESS_PEER_MAP_PATH), _("bpf obj get: %s"), strret);
  err = try2(set_egress_peer_map(egress_peer_map_fd, &egress_peer_key, &egress_peer_value), _("set_egress_peer_map: %s"),
             strret);

  ingress_peer_key = (typeof(ingress_peer_key)) {
    .uid = uid,
  };
  ingress_peer_value = (typeof(ingress_peer_value)) {
    .port = htons(port),
  };
  ingress_peer_key.address = in6;

  ingress_peer_map_fd = try2(bpf_obj_get(INGRESS_PEER_MAP_PATH), _("bpf obj get: %s"), strret);
  err = try2(set_ingress_peer_map(ingress_peer_map_fd, &ingress_peer_key, &ingress_peer_value), _("set_ingress_peer_map: %s"),
             strret);

  {
    char ipstr[INET6_ADDRSTRLEN], *uidstr = NULL;
    try2(ipv6_ntop(ipstr, &in6), "ipv6_ntop: %s", strret);
    try2(uid2string(uid, &uidstr, 0), "uid2string: %s", strret);
    log_info("client set: %s, address: %s, port: %u, comment: %.*s", uidstr, ipstr, port,
             (int) sizeof(egress_peer_value.comment), egress_peer_value.comment);
    free(uidstr);
  }

err_cleanup:
  if (egress_peer_map_fd >= 0)
    close(egress_peer_map_fd);
  if (ingress_peer_map_fd >= 0)
    close(ingress_peer_map_fd);
  return err;
}

#define CMD_CLIENT_DEL_SUMMARY "Remove a peer from the client configuration"

static int print_client_del_usage(int argc, char **argv) {
  (void) argc;
  fprintf(stderr,
          "Usage: %s %s [OPTIONS] {uid UID | user USERNAME} address ADDRESS\n\n"

          "  " CMD_CLIENT_DEL_SUMMARY ".\n\n"

          "Arguments:\n"
          "  You must specify either a uid or a username, as well as the address, to delete the peer.\n\n"

          "  %-24s The numeric user ID of the peer to delete.\n"
          "  %-24s The username of the peer to delete.\n"
          "  %-24s The server address associated with the peer.\n"
          "  %-24s This uniquely identifies the configuration to delete.\n\n"

          "Options:\n"
          "  %-22s Display UID as a number instead of resolving it to a username"
          " in command output. \n",

          STR(PROG_NAME), argv[0], "uid UID", "user USERNAME", "address ADDRESS", "", "-n");
  return 0;
}

int cmd_client_del(int argc, char **argv) {
  const char     *address             = NULL;
  uint8_t         uid                 = 0;
  bool            uid_set             = false;
  int             egress_peer_map_fd  = -1;
  int             ingress_peer_map_fd = -1;
  int             config_map_fd       = -1;
  struct in6_addr in6;
  int             err       = -EINVAL;
  bool            is_server = false;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (is_address_kw(tok)) {
      if (++i >= argc)
        goto usage;

      address = argv[i];
    } else if (is_user_kw(tok)) {
      if (++i >= argc)
        goto usage;

      try(string2uid(argv[i], &uid));
      uid_set = true;
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      /* 未知关键字 */
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_client_del_usage(argc, argv), -EINVAL;
    }
  }

  try2(get_server_mode(&is_server));
  if (is_server) {
    log_error("You must be in client mode to delete peers.");
    err = -EINVAL;
    goto err_cleanup;
  }

  // 检查必要参数
  if (!address || !uid_set) {
    log_error("UID and address must be specified.");
    goto usage;
  }

  try2(resolve_ip_addr(family, address, &in6));
  bool deleted = false;

  egress_peer_map_fd = try2(bpf_obj_get(EGRESS_PEER_MAP_PATH), _("bpf_obj_get: %s: %s"), EGRESS_PEER_MAP_PATH, strret);
  BPF_MAP_FOREACH(egress_peer_map_fd, struct egress_peer_key, struct egress_peer_value, key, value, {
    if (uid == value.uid && !memcmp(&key.address, &in6, sizeof(in6))) {
      bpf_map_delete_elem(egress_peer_map_fd, &key);
      deleted = true;
      break;
    }
  });

  ingress_peer_map_fd = try2(bpf_obj_get(INGRESS_PEER_MAP_PATH), _("bpf_obj_get: %s: %s"), INGRESS_PEER_MAP_PATH, strret);
  BPF_MAP_FOREACH(ingress_peer_map_fd, struct ingress_peer_key, struct ingress_peer_value, key, value, {
    if (uid == key.uid && !memcmp(&key.address, &in6, sizeof(in6))) {
      bpf_map_delete_elem(ingress_peer_map_fd, &key);
      break;
    }
  });

  char *uidstr = NULL;
  try2(uid2string(uid, &uidstr, 0), "uid2string: %s", strret);
  if (deleted)
    log_info("client deleted: %s", uidstr);
  else
    log_info("client peer: %s address %s not found", uidstr, address);
  free(uidstr);

err_cleanup:
  if (ingress_peer_map_fd >= 0)
    close(ingress_peer_map_fd);
  if (egress_peer_map_fd >= 0)
    close(egress_peer_map_fd);
  if (config_map_fd >= 0)
    close(config_map_fd);

  return err;
}

#define CMD_SERVER_ADD_SUMMARY "Add a peer to the server configuration"

static int print_server_add_usage(int argc, char **argv) {
  (void) argc;
  fprintf(
    stderr,
    "Usage: %s %s [OPTIONS] {uid UID | user USERNAME} address ADDR port PORT [icmp-id ID] [sport PORT] [comment COMMENT]\n\n"

    "  " CMD_SERVER_ADD_SUMMARY ".\n\n"

    "Arguments:\n"
    "  You must specify the client to add by providing EITHER a uid OR a username.\n\n"

    "  %-22s The numeric user ID to authorize.\n"
    "  %-22s The username to authorize.\n"
    "  %-22s The client's source address.\n"
    "  %-22s The client's source port for the tunnel.\n"
    "  %-22s Optional: The specific ICMP ID for the client.\n"
    "  %-22s Optional: The specific source port for the client.\n"
    "  %-22s Optional: A descriptive comment for this client entry.\n\n"

    "Options:\n"
    "  %-22s When resolving a hostname in 'address', force IPv4.\n"
    "  %-22s When resolving a hostname in 'address', force IPv6.\n"
    "  %-22s Display UID as a number instead of resolving it to a username"
    " in command output. \n",

    STR(PROG_NAME), argv[0], "uid UID", "user USERNAME", "address ADDR", "port PORT", "icmp-id ICMP_ID", "sport PORT",
    "comment COMMENT", "-4", "-6", "-n");
  return 0;
}

int cmd_server_add(int argc, char **argv) {
  uint8_t     uid         = 0;
  const char *address     = NULL;
  uint16_t    sport       = 0;
  uint16_t    port        = 0;
  uint16_t    icmp_id     = 0;
  bool        uid_set     = false;
  char       *comment     = NULL;
  int         user_map_fd = -1;
  int         err         = -EINVAL;
  bool        is_server   = false;

  struct user_info user;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (is_user_kw(tok)) {
      if (++i >= argc)
        goto usage;

      try(string2uid(argv[i], &uid));
      uid_set = true;
    } else if (is_address_kw(tok)) {
      if (++i >= argc)
        goto usage;

      address = argv[i];
    } else if (matches(tok, "sport") == 0) {
      if (++i >= argc)
        goto usage;

      try(parse_sport(argv[i], &sport));
    } else if (matches(tok, "icmp-id") == 0) {
      if (++i >= argc)
        goto usage;

      try(parse_icmp_id(argv[i], &icmp_id));
    } else if (matches(tok, "port") == 0) {
      if (++i >= argc)
        goto usage;

      try(parse_port(argv[i], &port));
    } else if (matches(tok, "comment") == 0) {
      if (++i >= argc)
        goto usage;

      comment = argv[i];
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      /* 未知关键字 */
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_server_add_usage(argc, argv), -EINVAL;
    }
  }

  /* 必要字段检查 */
  if (!uid_set || !address || !port) {
    log_error("uid, address and port must be specified");
    goto usage;
  }

  try2(get_server_mode(&is_server));
  if (!is_server) {
    log_error("You must be in server mode to add peers.");
    err = -EINVAL;
    goto err_cleanup;
  }

  struct in6_addr client_addr = {};
  try(resolve_ip_addr(family, address, &client_addr));

  user_map_fd = try2(bpf_obj_get(USER_MAP_PATH), _("bpf_obj_get: %s: %s"), USER_MAP_PATH, strret);

  user = (typeof(user)) {
    .address = client_addr,
    .icmp_id = htons(icmp_id),
    .sport   = htons(sport),
    .dport   = htons(port),
  };

  if (comment) {
    if (strpbrk(comment, "\r\n")) {
      log_error("Multi-line comment is not allowed.");
      err = -EINVAL;
      goto err_cleanup;
    }

    strncpy((char *) user.comment, comment, sizeof(user.comment));
    user.comment[sizeof(user.comment) - 1] = '\0';
  }

  err = try2(bpf_map_update_elem(user_map_fd, &uid, &user, 0), _("update user map: %s"), strret);

  {
    char  ipstr[INET6_ADDRSTRLEN];
    char *uidstr = NULL;
    try2(ipv6_ntop(ipstr, &user.address), "ipv6_ntop: %s %s", ipstr, strret);
    try2(uid2string(uid, &uidstr, 0), "uid2string: %s", strret);
    log_info("server updated: %s, address: %s, dport: %u, comment: %.*s", uidstr, ipstr, ntohs(user.dport),
             (int) sizeof(user.comment), user.comment);
    free(uidstr);
  }

err_cleanup:
  if (user_map_fd >= 0)
    close(user_map_fd);
  return err;
}

#define CMD_SERVER_DEL_SUMMARY "Remove a peer from the server configuration"

static int print_server_del_usage(int argc, char **argv) {
  (void) argc;

  fprintf(stderr,
          "Usage: %s %s [OPTIONS] {uid <id> | user <name>}\n\n"

          "  " CMD_SERVER_DEL_SUMMARY ".\n\n"

          "Arguments:\n"
          "  You must specify a user to delete using exactly one of the following identifiers:\n\n"
          "  %-22s Specifies the target user by their numerical User ID (UID).\n"
          "  %-22s Specifies the target user by their username.\n\n"

          "Options:\n"
          "  %-22s Display UID as a number instead of resolving it to a username"
          " in command output. \n",

          STR(PROG_NAME), argv[0], "uid <id>", "user <name>", "-n");
  return 0;
}

int cmd_server_del(int argc, char **argv) {
  uint8_t          uid     = 0;
  bool             uid_set = false;
  struct user_info user;
  int              user_map_fd = -1;
  int              err         = -EINVAL;
  bool             is_server   = false;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (is_user_kw(tok)) {
      if (++i >= argc)
        goto usage;

      try(string2uid(argv[i], &uid));
      uid_set = true;
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      /* 未知关键字 */
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_server_del_usage(argc, argv), -EINVAL;
    }
  }

  // 检查必要参数
  if (!uid_set) {
    log_error("UID must be specified.");
    goto usage;
  }

  try2(get_server_mode(&is_server));
  if (!is_server) {
    log_error("You must be in server mode to delete peers.");
    err = -EINVAL;
    goto err_cleanup;
  }

  user_map_fd = try2(bpf_obj_get(USER_MAP_PATH), _("bpf_obj_get: %s: %s"), USER_MAP_PATH, strret);
  try2(bpf_map_lookup_elem(user_map_fd, &uid, &user), _("bpf_map_lookup_elem user_map uid: %u: %s"), uid, strret);
  err = try2(bpf_map_delete_elem(user_map_fd, &uid), _("bpf_map_delete_elem user_map: %s"), strret);

  char *uidstr = NULL;
  try2(uid2string(uid, &uidstr, 0), "uid2string: %s", strret);
  log_info("server deleted: %s", uidstr);
  free(uidstr);
err_cleanup:
  if (user_map_fd >= 0)
    close(user_map_fd);

  return err;
}

#define CMD_STATUS_SUMMARY "Display the current status and active sessions"

static int print_status_usage(int argc, char *argv[]) {
  (void) argc;

  fprintf(stderr,
          "Usage: %s %s [OPTIONS] [debug]\n\n"

          "  " CMD_STATUS_SUMMARY ".\n\n"

          "Arguments:\n"
          "  %-22s Optional: If specified, displays detailed debugging information.\n\n"

          "Options:\n"
          "  %-22s Display UID as a number instead of resolving it to a username"
          " in command output. \n",

          STR(PROG_NAME), argv[0], "debug", "-n");
  return 0;
}

static void print_escaped(const char *str, size_t len) {
  for (size_t i = 0; i < len && str[i] != '\0'; ++i) {
    char c = str[i];
    // Escape characters that are special inside double quotes
    if (c == '"' || c == '\\' || c == '$' || c == '\'') {
      putchar('\\');
    }
    putchar(c);
  }
}

static void print_comment(const char *comment, size_t comment_len, int dsl) {
  if (!comment[0])
    return;

  int needs_quoting = !!strpbrk(comment, special_chars);

  printf(dsl ? " comment " : ", Comment: ");

  if (needs_quoting) {
    putchar('"');
    print_escaped(comment, comment_len);
    putchar('"');
  } else {
    printf("%.*s", (int) comment_len, comment);
  }
}

int cmd_status(int argc, char **argv) {
  int config_map_fd       = -1;
  int build_type_map_fd   = -1;
  int user_map_fd         = -1;
  int session_map_fd      = -1;
  int egress_peer_map_fd  = -1;
  int ingress_peer_map_fd = -1;
  int err                 = 0;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (matches(tok, "debug") == 0) {
      debug = 1;
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_status_usage(argc, argv), -EINVAL;
    }
  }

  config_map_fd = bpf_obj_get(CONFIG_MAP_PATH);
  if (config_map_fd < 0) {
    switch (errno) {
    case EPERM:
    case EACCES:
      log_error("you must be root to run %s", STR(PROG_NAME));
      break;
    case ENOENT:
      log_error("%s is not loaded", STR(PROJECT_NAME));
      break;
    default:
      log_error("config_map_fd: bpf_obj_get: %s", strerror(errno));
      break;
    }
    err = errno;
    goto err_cleanup;
  }

  struct config cfg = {};
  try2(get_config_map(config_map_fd, &cfg), _("get_config_map: %s"), strret);

  {
    uint32_t build_type = 0;

    build_type_map_fd = try2(bpf_obj_get(BUILD_TYPE_MAP_PATH), _("bpf_obj_get: build_type_map: %s"), strret);
    try2(get_build_type_map(build_type_map_fd, &build_type), _("get build type map: %s"), strret);
    printf("tutuicmptunnel: Role: %s, BPF build type: %s, no-fixup: %s\n\n", cfg.is_server ? "Server" : "Client",
           build_type ? "Debug" : "Release", cfg.no_fixup ? "on" : "off");
  }

  if (cfg.is_server) {
    printf("Peers:\n");
    // 打印所有peer
    user_map_fd = try2(bpf_obj_get(USER_MAP_PATH), _("bpf_obj_get: user_map: %s"), strret);
    BPF_MAP_FOREACH(user_map_fd, uint8_t, struct user_info, key, value, {
      char            ipstr[INET6_ADDRSTRLEN];
      struct in6_addr in6 = value.address;

      try2(ipv6_ntop(ipstr, &in6), "ipv6_ntop: %s %s", ipstr, strret);
      char *uidstr = NULL;
      try2(uid2string(key, &uidstr, 0), "uid2string: %s", strret);
      printf("  %s, Address: %s, "
             "Sport: %u, Dport: %u, ICMP: %u",
             uidstr, ipstr, ntohs(value.sport), ntohs(value.dport), ntohs(value.icmp_id));
      free(uidstr);

      print_comment((const char *) value.comment, sizeof(value.comment), 0);
      printf("\n");
    });

    if (debug) {
      printf("\nSessions (max age: %u):\n", cfg.session_max_age);
      session_map_fd = try2(bpf_obj_get(SESSION_MAP_PATH), _("bpf_obj_get: %s: %s"), SESSION_MAP_PATH, strret);
      {
        BPF_MAP_FOREACH(session_map_fd, struct session_key, struct session_value, key, value, {
          char            ipstr[INET6_ADDRSTRLEN];
          struct in6_addr in6 = key.address;

          try2(ipv6_ntop(ipstr, &in6), "ipv6_ntop: %s", strret);
          char *uidstr = NULL;
          try2(uid2string(value.uid, &uidstr, 0), "uid2string: %s", strret);
          printf("  Address: %s, SPort: %u, DPort: %u => "
                 "%s, Age: %llu\n",
                 ipstr, ntohs(key.sport), ntohs(key.dport), uidstr, value.age);
          free(uidstr);
        });
      }
    }
  } else { /* client 角色 */
    int cnt = 0;

    egress_peer_map_fd = try2(bpf_obj_get(EGRESS_PEER_MAP_PATH), _("bpf_obj_get: %s: %s"), EGRESS_PEER_MAP_PATH, strret);
    printf("Peers: \n");
    BPF_MAP_FOREACH(egress_peer_map_fd, struct egress_peer_key, struct egress_peer_value, key, value, {
      if (ntohs(key.port)) {
        char            ipstr[INET6_ADDRSTRLEN];
        struct in6_addr in6 = key.address;

        try2(ipv6_ntop(ipstr, &in6), "ipv6_ntop: %s", strret);
        char *uidstr = NULL;
        try2(uid2string(value.uid, &uidstr, 0), "uid2string: %s", strret);
        printf("  %s, Address: %s, Port: %u", uidstr, ipstr, ntohs(key.port));
        free(uidstr);
        print_comment((const char *) value.comment, sizeof(value.comment), 0);
        printf("\n");
        cnt++;
      }
    });

    if (!cnt) {
      printf("No peer configure\n");
    }

    if (debug) {
      printf("\nIngress peers:\n");
      ingress_peer_map_fd = try2(bpf_obj_get(INGRESS_PEER_MAP_PATH), _("bpf_obj_get: %s: %s"), INGRESS_PEER_MAP_PATH, strret);
      BPF_MAP_FOREACH(ingress_peer_map_fd, struct ingress_peer_key, struct ingress_peer_value, key, value, {
        char  ipstr[INET6_ADDRSTRLEN];
        char *uidstr = NULL;

        try2(ipv6_ntop(ipstr, &key.address), "ipv6_ntop: %s", strret);
        try2(uid2string(key.uid, &uidstr, 0), "uid2string: %s", strret);
        printf("  %s, Address: %s => Sport: %u\n", uidstr, ipstr, htons(value.port));
        free(uidstr);
      });
    }
  }

  if (debug) {
    printf("\nPackets:\n");
    printf("  processed:   %8llu\n", cfg.packets_processed);
    printf("  dropped:     %8llu\n", cfg.packets_dropped);
    printf("  cksum error: %8llu\n", cfg.checksum_errors);
    printf("  fragmented:  %8llu\n", cfg.fragmented);
    printf("  GSO:         %8llu\n", cfg.gso);
  }

  err = 0;

err_cleanup:
  if (ingress_peer_map_fd >= 0)
    close(ingress_peer_map_fd);
  if (egress_peer_map_fd >= 0)
    close(egress_peer_map_fd);
  if (session_map_fd >= 0)
    close(session_map_fd);
  if (user_map_fd >= 0)
    close(user_map_fd);
  if (build_type_map_fd >= 0)
    close(build_type_map_fd);
  if (config_map_fd >= 0)
    close(config_map_fd);
  return err;
}

#define CMD_DUMP_SUMMARY "Dump the current running configuration in a raw format"

static int print_dump_usage(int argc, char *argv[]) {
  (void) argc;

  fprintf(stderr,
          "Usage: %s %s [OPTIONS]\n\n"

          "  " CMD_DUMP_SUMMARY ".\n\n"

          "Options:\n"
          "  %-22s Display UID as a number instead of resolving it to a username"
          " in command output. \n",

          STR(PROG_NAME), argv[0], "-n");
  return 0;
}

int cmd_dump(int argc, char **argv) {
  int           config_map_fd = -1;
  int           user_map_fd   = -1;
  int           peer_map_fd   = -1;
  int           err           = 0;
  struct config cfg           = {};

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (is_help_kw(tok)) {
      goto usage;
    } else {
      log_error("unknown keyword \"%s\"", tok);
    usage:
      return print_dump_usage(argc, argv), -EINVAL;
    }
  }

  config_map_fd = try2(bpf_obj_get(CONFIG_MAP_PATH), "bpf_obj_get: %s: %s", CONFIG_MAP_PATH, strret);
  try2(get_config_map(config_map_fd, &cfg), _("get_config_map: %s"), strret);

  {
    time_t    now = time(NULL);
    char      buf[64];
    struct tm tm = *localtime(&now);
    strftime(buf, sizeof(buf), "%F %T %Z", &tm);
    buf[sizeof(buf) - 1] = '\0';

    printf("#!%s/sbin/tuctl script -\n", STR(INSTALL_PREFIX_DIR));
    printf("# Auto-generated by \"tuctl dump\" on %s\n\n", buf);
  }

  if (cfg.is_server) {
    printf("server max-age %u%s\n\n", cfg.session_max_age, cfg.no_fixup ? " no-fixup" : "");
    user_map_fd = try2(bpf_obj_get(USER_MAP_PATH), _("bpf_obj_get: user_map: %s"), strret);
    BPF_MAP_FOREACH(user_map_fd, uint8_t, struct user_info, key, value, {
      char            ipstr[INET6_ADDRSTRLEN];
      struct in6_addr in6    = value.address;
      char           *uidstr = NULL;

      try2(ipv6_ntop(ipstr, &in6), "ipv6_ntop: %s", strret);
      try2(uid2string(key, &uidstr, 1), "uid2string: %s", strret);
      printf("server-add "
             "%s "
             "addr %s "
             "icmp-id %u "
             "sport %u "
             "port %u",
             uidstr, ipstr, ntohs(value.icmp_id), ntohs(value.sport), ntohs(value.dport));
      free(uidstr);

      print_comment((const char *) value.comment, sizeof(value.comment), 1);
      printf("\n");
    });
  } else {
    printf("client%s\n", cfg.no_fixup ? " no-fixup" : "");
    peer_map_fd = try2(bpf_obj_get(EGRESS_PEER_MAP_PATH), _("bpf_obj_get: %s: %s"), EGRESS_PEER_MAP_PATH, strret);
    BPF_MAP_FOREACH(peer_map_fd, struct egress_peer_key, struct egress_peer_value, key, value, {
      if (ntohs(key.port)) {
        char            ipstr[INET6_ADDRSTRLEN];
        struct in6_addr in6 = key.address;

        try2(ipv6_ntop(ipstr, &in6), "ipv6_ntop: %s", strret);
        char *uidstr = NULL;
        try2(uid2string(value.uid, &uidstr, 1), "uid2string: %s", strret);
        printf("client-add "
               "%s "
               "addr %s "
               "port %u",
               uidstr, ipstr, ntohs(key.port));
        free(uidstr);
        print_comment((const char *) value.comment, sizeof(value.comment), 1);
        printf("\n");
      }
    });
  }

  err = 0;

err_cleanup:
  if (peer_map_fd >= 0)
    close(peer_map_fd);
  if (user_map_fd >= 0)
    close(user_map_fd);
  if (config_map_fd >= 0)
    close(config_map_fd);
  return err;
}

#define CMD_REAPER_SUMMARY "Cleanup stale NAT sessions"
#define CMD_SCRIPT_SUMMARY "Execute a batch of commands from a file or standard input"

// clang-format off
static subcommand_t subcommands[] = {
  { "load", cmd_load, "Load and attach BPF programs", },
  { "unload", cmd_unload, "Detach and unload BPF programs", },
  { "server", cmd_server, CMD_SERVER_SUMMARY, },
  { "client", cmd_client, CMD_CLIENT_SUMMARY, },
  { "client-add", cmd_client_add, CMD_CLIENT_ADD_SUMMARY, },
  { "client-del", cmd_client_del, CMD_CLIENT_DEL_SUMMARY, },
  { "server-add", cmd_server_add, CMD_SERVER_ADD_SUMMARY, },
  { "server-del", cmd_server_del, CMD_SERVER_DEL_SUMMARY, },
  { "reaper", cmd_reaper, CMD_REAPER_SUMMARY, },
  { "status", cmd_status, CMD_STATUS_SUMMARY, },
  { "version", cmd_version, "Show program version", },
  { "dump", cmd_dump, CMD_DUMP_SUMMARY, },
  { "script", cmd_script, CMD_SCRIPT_SUMMARY, },
  { "help", cmd_help, "Show this help message"},
  {NULL, NULL, NULL},
};
// clang-format on

static void print_help_usage(void) {
  printf("Usage: %s <subcommand> [options]\n", STR(PROG_NAME));
  printf("Subcommands:\n");
  for (int i = 0; subcommands[i].name; ++i) {
    printf("  %-10s  %s\n", subcommands[i].name, subcommands[i].desc);
  }
}

static int dispatch(int argc, char **argv) {
  if (argc <= 0 || !argv || !argv[0])
    return -EINVAL;

  for (subcommand_t *sc = subcommands; sc->name; ++sc) {
    if (!strcmp(argv[0], sc->name)) {
      return sc->handler(argc, argv);
    }
  }

  return log_error("error: unknown command '%s'", argv[0]), print_help_usage(), -EINVAL;
}

static int print_script_usage(int argc, char **argv) {
  (void) argc;
  fprintf(stderr,
          "Usage: %s %s [<file>|-]\n\n"

          "  " CMD_SCRIPT_SUMMARY ".\n\n"

          "Arguments:\n"
          "  %-22s The path to a script file containing commands to be executed.\n"
          "  %-22s If specified, commands will be read from standard input (stdin).\n\n",

          STR(PROG_NAME), argv[0], "<file>", "-");
  return 0;
}

int cmd_script(int argc, char **argv) {
  FILE       *fp   = NULL;
  const char *path = "-";
  char       *line = NULL;
  size_t      len  = 0;
  int         lnum = 0;

  int err = 0;

  if (help || argc > 2) {
    return print_script_usage(argc, argv), -EINVAL;
  }

  if (argc == 2)
    path = argv[1];
  if (!strcmp(path, "-")) {
    fp = stdin;
  } else {
    fp = try2_p(fopen(path, "r"), "fopen: %s", strret);
  }

  while (getline(&line, &len, fp) != -1) {
    ++lnum;
    strip_inline_comment(line);
    LIST_HEAD(args);
    try2(split_args_list(line, &args), "parse error at line %d: %s\n", lnum, line);

    if (list_empty(&args))
      continue;

    int    my_argc = 0;
    char **my_argv = NULL;
    try2(args_list_to_argv(&args, &my_argc, &my_argv));

    err = dispatch(my_argc, my_argv);
    free(my_argv);
    free_args_list(&args);
    if (err)
      break;
  }

err_cleanup:
  if (fp && fp != stdin)
    fclose(fp);
  free(line);
  return err;
}

struct iface_node {
  char             name[IFNAMSIZ];
  struct list_head head;
};

static int add_iface(struct list_head *iface_list_head, const char *iface) {
  struct iface_node *new_iface = (typeof(new_iface)) malloc(sizeof(*new_iface));

  if (!new_iface)
    return -ENOMEM;

  strncpy(new_iface->name, iface, sizeof(new_iface->name));
  new_iface->name[sizeof(new_iface->name) - 1] = '\0';
  list_add_tail(&new_iface->head, iface_list_head);
  return 0;
}

static const char *link_type_str(int link_type) {
  switch (link_type) {
  case LINK_AUTODETECT:
    return "auto-detect";
  case LINK_ETH:
    return "ethernet header";
  case LINK_NONE:
    return "no ethernet header";
  default:
    break;
  }

  return "unknown";
}

static int already_pinned(void) {
  char        path[PATH_MAX];
  struct stat s;

  snprintf(path, sizeof(path), "%s%s", PIN_DIR, "handle_egress");
  if (stat(path, &s) == 0) {
    return 1;
  }

  return 0;
}

static int attach_tc_bpf(int ifindex, int prog_fd, enum bpf_tc_attach_point point) {
  int                err  = 0;
  struct bpf_tc_hook hook = {
    .sz           = sizeof(hook),
    .ifindex      = ifindex,
    .attach_point = point,
  };
  struct bpf_tc_opts opts = {
    .sz       = sizeof(opts),
    .handle   = TC_HANDLE_DEFAULT,
    .priority = TC_PRIORITY_DEFAULT,
    .prog_fd  = prog_fd,
  };

  err = bpf_tc_hook_create(&hook);
  if (err && err != -EEXIST) {
    return err;
  }

  err = bpf_tc_attach(&hook, &opts);
  if (err && err != -EEXIST) {
    return err;
  }

  return 0;
}

static int detach_tc_bpf(int ifindex, enum bpf_tc_attach_point point) {
  struct bpf_tc_hook hook = {
    .sz           = sizeof(hook),
    .ifindex      = ifindex,
    .attach_point = point,
  };
  struct bpf_tc_opts opts = {
    .sz       = sizeof(opts),
    .handle   = TC_HANDLE_DEFAULT,
    .priority = TC_PRIORITY_DEFAULT,
  };

  try(bpf_tc_detach(&hook, &opts));
  try(bpf_tc_hook_destroy(&hook));

  return 0;
}

static int print_load_usage(int argc, char **argv) {
  (void) argc;
  fprintf(stderr,
          "Usage: %s %s [OPTIONS] IFACE [IFACE...]\n\n"

          "  Loads the BPF program and attaches it to the 'clsact' qdisc on one or more\n"
          "  network interfaces. This is the core step to enable %s.\n\n"

          "Arguments:\n"
          "  %-22s One or more network interface names (e.g., eth0, ppp0)\n"
          "  %-22s on which to load the BPF program.\n\n"

          "Options:\n"
          "  By default, the program auto-detects whether an Ethernet header is present.\n"
          "  Use the following options to override this behavior. 'ethhdr' and 'no-ethhdr' are mutually exclusive.\n\n"

          "  %-22s Loads the debug build of the BPF program into the kernel.\n"
          "  %-22s Force the BPF program to assume an Ethernet header is present.\n"
          "  %-22s Force the BPF program to assume no Ethernet header is present\n"
          "  %-22s (e.g., for PPPoE interfaces).\n\n",

          STR(PROG_NAME), argv[0], STR(PROJECT_NAME), "IFACE [IFACE...]", "", "debug", "ethhdr", "no-ethhdr", "");

  return 0;
}

int cmd_load(int argc, char **argv) {
  struct tutuicmptunnel       *bpf       = NULL;
  struct tutuicmptunnel_debug *bpf_debug = NULL;
  LIST_HEAD(iface_list_head);
  struct iface_node *node, *tmp_node;
  int                build_type_map_fd = -1;
  int                link_type         = LINK_AUTODETECT; /* 默认自动探测 */
  int                err               = -EINVAL;
  int                ingress_fd = -1, egress_fd = -1;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (strcasecmp(tok, "debug") == 0) {
      debug = 1;
    } else if (strcasecmp(tok, "ethhdr") == 0) {
      link_type = LINK_ETH;
    } else if (strcasecmp(tok, "no-ethhdr") == 0) {
      link_type = LINK_NONE;
    } else if (strcasecmp(tok, "iface") == 0) {
      if (i + 1 >= argc) {
        log_error("\"iface\" keyword needs an argument.");
        goto usage;
      }

      const char *ifname = argv[++i];
      try2(add_iface(&iface_list_head, ifname), "add_iface: %s", strret);
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      log_error("unknown keyword \"%s\"", tok);
    usage:
      print_load_usage(argc, argv);
      err = -EINVAL;
      goto err_cleanup;
    }
  }

  /* 至少要指定一个 iface */
  if (list_empty(&iface_list_head)) {
    log_error("At least one interface name must be specified with \"iface\".");
    goto usage;
  }

  if (!already_pinned()) {
#if LIBBPF_VERSION_GEQ(0, 6)
    struct bpf_program *egress = NULL, *ingress = NULL;
#endif

    fprintf(stderr, "loading %s BPF program into the kernel\n", debug ? "debug" : "release");

    if (debug)
      bpf_debug = try2_p(tutuicmptunnel_debug__open_and_load(), _("Failed to open and load BPF skeleton: %s"), strret);
    else
      bpf = try2_p(tutuicmptunnel__open_and_load(), _("Failed to open and load BPF skeleton: %s"), strret);

    fprintf(stderr, "link type set to %s.\n", link_type_str(link_type));

    if (debug) {
      bpf_debug->data->link_type = link_type;
#if LIBBPF_VERSION_GEQ(0, 6)
      egress  = bpf_debug->progs.handle_egress;
      ingress = bpf_debug->progs.handle_ingress;
#endif
    } else {
      bpf->data->link_type = link_type;
#if LIBBPF_VERSION_GEQ(0, 6)
      egress  = bpf->progs.handle_egress;
      ingress = bpf->progs.handle_ingress;
#endif
    }

#if LIBBPF_VERSION_GEQ(0, 6)
    bpf_program__set_flags(egress, BPF_F_ANY_ALIGNMENT);
    bpf_program__set_flags(ingress, BPF_F_ANY_ALIGNMENT);
#endif
    if (debug)
      try2(auto_pin_debug(bpf_debug), _("auto_pin_debug: %s"), strret);
    else
      try2(auto_pin(bpf), _("auto_pin: %s"), strret);

    build_type_map_fd = try2(bpf_obj_get(BUILD_TYPE_MAP_PATH), _("bpf_obj_get: %s: %s"), BUILD_TYPE_MAP_PATH, strret);
    try2(set_build_type_map(build_type_map_fd, debug), "set build type map: %s", strret);
  }

  ingress_fd = try2(bpf_obj_get(HANDLE_INGRESS_PATH), _("bpf_obj_get: %s: %s"), HANDLE_INGRESS_PATH, strret);
  egress_fd  = try2(bpf_obj_get(HANDLE_EGRESS_PATH), _("bpf_obj_get: %s: %s"), HANDLE_EGRESS_PATH, strret);

  /* 针对每个 iface 做 tc 配置并挂载 BPF */
  list_for_each_entry(node, &iface_list_head, head) {
    const char *cur_iface = node->name;
    int         ifindex;

    log_info("Configuring TC for interface: %s", cur_iface);

    ifindex = if_nametoindex(cur_iface);
    if (ifindex == 0) {
      log_error("Invalid interface: %s", cur_iface);
      continue;
    }

    err = attach_tc_bpf(ifindex, ingress_fd, BPF_TC_INGRESS);
    if (err) {
      log_error("Failed to attach ingress on %s: %s", cur_iface, strerror(-err));
      continue;
    }

    err = attach_tc_bpf(ifindex, egress_fd, BPF_TC_EGRESS);
    if (err) {
      log_error("Failed to attach egress on %s", cur_iface, strerror(-err));
      continue;
    }
  }

  err = 0;
err_cleanup:
  if (egress_fd >= 0)
    close(egress_fd);
  if (ingress_fd >= 0)
    close(ingress_fd);
  list_for_each_entry_safe(node, tmp_node, &iface_list_head, head) {
    list_del(&node->head);
    free(node);
  }
  if (build_type_map_fd >= 0)
    close(build_type_map_fd);
  if (bpf)
    tutuicmptunnel__destroy(bpf);
  if (bpf_debug)
    tutuicmptunnel_debug__destroy(bpf_debug);
  return err;
}

static int print_unload_usage(int argc, char **argv) {
  (void) argc;
  fprintf(stderr,
          "Usage: %s %s IFACE [IFACE...]\n\n"

          "  Detaches the BPF program from the 'clsact' qdisc on the specified interfaces.\n"
          "  It then immediately and permanently deletes the BPF program by unpinning it\n"
          "  from the filesystem.\n\n"

          "Arguments:\n"
          "  %-22s One or more network interface names (e.g., eth0, ppp0)\n"
          "  %-22s from which to unload the BPF program.\n\n",

          STR(PROG_NAME), argv[0], "IFACE [IFACE...]", "");
  return 0;
}

int cmd_unload(int argc, char **argv) {
  int err = -EINVAL;
  LIST_HEAD(iface_list_head);
  struct iface_node *node, *tmp_node;

  if (help)
    goto usage;

  for (int i = 1; i < argc; ++i) {
    const char *tok = argv[i];

    if (strcasecmp(tok, "iface") == 0) {
      if (i + 1 >= argc) {
        log_error("\"iface\" keyword needs an argument.");
        goto usage;
      }

      const char *ifname = argv[++i];
      try2(add_iface(&iface_list_head, ifname), "add_iface: %s", strret);
    } else if (is_help_kw(tok)) {
      goto usage;
    } else {
      log_error("unknown keyword \"%s\"", tok);
    usage:
      print_unload_usage(argc, argv);
      err = -EINVAL;
      goto err_cleanup;
    }
  }

  if (list_empty(&iface_list_head)) {
    log_error("At least one interface name must be specified with \"iface\".");
    goto usage;
  }

  list_for_each_entry(node, &iface_list_head, head) {
    const char *cur_iface = node->name;
    int         ifindex;

    log_info("Deconfiguring TC for interface: %s", cur_iface);
    ifindex = if_nametoindex(cur_iface);
    if (ifindex == 0) {
      log_error("Invalid interface: %s", cur_iface);
      err = -ENOENT;
      goto err_cleanup;
    }

    try2(detach_tc_bpf(ifindex, BPF_TC_INGRESS), _("detach_tc_bpf: %s %s"), "ingress", strret);
    try2(detach_tc_bpf(ifindex, BPF_TC_EGRESS), _("detach_tc_bpf: %s %s"), "egress", strret);
  }

  err = unauto_pin();
  if (err) {
    /* 忽略“未找到” */
    if (errno == ENOENT) {
      err = 0;
    } else {
      log_error("unauto_pin failed: %s", strerror(errno));
    }
  }

err_cleanup:
  list_for_each_entry_safe(node, tmp_node, &iface_list_head, head) {
    list_del(&node->head);
    free(node);
  }

  return err;
}

static __u64 get_monotonic_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (__u64) ts.tv_sec;
}

static int print_reaper_usage(int argc, char *argv[]) {
  (void) argc;

  fprintf(stderr,
          "Usage: %s %s [OPTIONS]\n\n"

          "  " CMD_REAPER_SUMMARY ".\n\n"
          "%s\n",
          STR(PROG_NAME), argv[0],
#ifdef DISABLE_BPF_TIMER
          ""
#else
          "  This command is OBSOLETE. Server no longer needs it."
#endif
  );
  return 0;
}

int cmd_reaper(int argc, char **argv) {
  int           session_map_fd = -1;
  int           config_map_fd  = -1;
  struct config cfg            = {};

  if (help) {
    return print_reaper_usage(argc, argv), -EINVAL;
  }

#ifndef DISABLE_BPF_TIMER
  log_warn("This command is OBSOLETE. Server no longer needs it.");
#endif

  int   err = 0;
  __u64 now = get_monotonic_seconds();

  config_map_fd = try2(bpf_obj_get(CONFIG_MAP_PATH), "bpf_get_obj %s: %s", CONFIG_MAP_PATH, strret);
  try2(get_config_map(config_map_fd, &cfg), _("get_config_map: %s"), strret);
  log_info("max allowed age: %u", cfg.session_max_age);
  session_map_fd = try2(bpf_obj_get(SESSION_MAP_PATH), _("bpf_obj_get: %s: %s"), SESSION_MAP_PATH, strret);

  BPF_MAP_FOREACH(session_map_fd, struct session_key, struct session_value, key, value, {
    char            ipstr[INET6_ADDRSTRLEN];
    struct in6_addr in6 = key.address;
    __u64           age;

    try2(ipv6_ntop(ipstr, &in6), "ipv6_ntop: %s", strret);
    age = value.age;
    if (!age || now - age > cfg.session_max_age) {
      char *uidstr = NULL;
      try2(uid2string(value.uid, &uidstr, 0), "uid2string: %s", strret);
      printf("Reaping old entry: Address: %s, DPort: %u, SPort: %u, %s, Age: %llu\n", ipstr, ntohs(key.dport), ntohs(key.sport),
             uidstr, age);
      free(uidstr);
      bpf_map_delete_elem(session_map_fd, &key);
    }
  });

  err = 0;
err_cleanup:
  if (session_map_fd >= 0)
    close(session_map_fd);
  if (config_map_fd >= 0)
    close(config_map_fd);
  return err;
}

int cmd_help(int argc, char **argv) {
  (void) argc;
  (void) argv;
  print_help_usage();
  return 0;
}

int cmd_version(int argc, char **argv) {
  (void) argc;
  (void) argv;
  printf("tutuicmptunnel: %s\n", VERSION_STR);
  return 0;
}

static int parse_argument(int argc, char **argv, int *subcmd_pos) {
  int                  err            = 0;
  static struct option long_options[] = {
    {"numeric", no_argument, 0, 'n'}, {"debug", no_argument, 0, 'd'}, {"ipv4", no_argument, 0, '4'},
    {"ipv6", no_argument, 0, '6'},    {"help", no_argument, 0, 'h'},  {0, 0, 0, 0},
  };

  int opt;

  if (subcmd_pos)
    *subcmd_pos = -1;

  while ((opt = getopt_long(argc, argv, "nd46h", long_options, NULL)) != -1) {
    switch (opt) {
    case 'n':
      log_info("using numeric output for UIDs");
      numeric = 1;
      break;
    case 'd':
      log_info("using debug mode");
      debug = 1;
      break;
    case '4':
      log_info("hostnames will be resolved to IPv4 addresses only");
      family = AF_INET;
      break;
    case '6':
      log_info("hostnames will be resolved to IPv6 addresses only");
      family = AF_INET6;
      break;
    case 'h':
      help = 1;
      break;
    default:
      break;
    }
  }

  if (subcmd_pos && optind < argc)
    *subcmd_pos = optind;

  if (help && *subcmd_pos < 0) {
    cmd_help(argc, argv);
    err = -EINVAL;
  }

  return err;
}

int main(int argc, char *argv[]) {
  int err, subcmd_pos;

  uid_map_init(&uids);
  // 忽略错误
  err = uid_map_load(&uids, UID_CONFIG_PATH);

  try(parse_argument(argc, argv, &subcmd_pos));

  if (subcmd_pos < 0) {
    char *newargv[] = {"status", NULL};

    err = cmd_status(ARRAY_SIZE(newargv) - 1, newargv);
    goto err_cleanup;
  }

  err = dispatch(argc - subcmd_pos, argv + subcmd_pos);
err_cleanup:
  uid_map_free(&uids);
  return err;
}

/* vim: set sw=2 expandtab : */
