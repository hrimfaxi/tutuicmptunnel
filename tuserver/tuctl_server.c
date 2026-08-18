#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <netdb.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../tucrypto/tucrypto.h"
#include "common.h"
#include "list.h"
#include "log.h"
#include "try.h"
#include "tuparser.h"

/* glibc 仅在 _DEFAULT_SOURCE 下声明 environ，musl 无条件声明；显式 extern 保证两边一致 */
extern char **environ;

/**
 * @brief Parses command-line arguments.
 * @return 0 on success, -EINVAL on error or if help is requested.
 */
static int parse_arguments(int argc, char **argv, const char **bind_addr, const char **port, char **psk, uint32_t *window,
                           uint32_t *replay_max) {
  int                  err            = 0;
  static struct option long_options[] = {{"bind", required_argument, 0, 'b'},
                                         {"port", required_argument, 0, 'p'},
                                         {"psk", required_argument, 0, 'k'},
                                         {"window", required_argument, 0, 'w'},
                                         {"replay-max", required_argument, 0, 'm'},
                                         {"help", no_argument, 0, 'h'},
                                         {0, 0, 0, 0}};

  int opt;
  while ((opt = getopt_long(argc, argv, "b:p:k:w:m:h", long_options, NULL)) != -1) {
    switch (opt) {
    case 'b':
      *bind_addr = optarg;
      break;
    case 'p':
      *port = optarg;
      break;
    case 'k':
      try(strdup_safe(optarg, psk));
      memset(optarg, '*', strlen(optarg));
      break;
    case 'w':
      try(parse_window(optarg, window));
      break;
    case 'm':
      try(parse_window(optarg, replay_max));
      break;
    case 'h':
    default:
      fprintf(stderr, "Usage: %s -k <psk> [--bind addr] [--port port] [--window window] [--replay-max n]\n", argv[0]);
      err = -EINVAL;
      goto err_cleanup;
    }
  }

  if (!*psk) {
    log_error("Missing required argument -k <psk>");
    fprintf(stderr, "Usage: %s -k <psk> [--bind addr] [--port port] [--window window] [--replay-max n]\n", argv[0]);
    err = -EINVAL;
    goto err_cleanup;
  }

  if (strlen(*psk) < 8) {
    log_error("PSK must be at least 8 characters long");
    fprintf(stderr, "Usage: %s -k <psk> [--bind addr] [--port port] [--window window] [--replay-max n]\n", argv[0]);
    err = -EINVAL;
    goto err_cleanup;
  }

err_cleanup:
  return err;
}

/**
 * @brief 创建并绑定一个 UDP socket。
 * @param[out] sock_out      用于返回创建成功的 socket 文件描述符。
 * @param[in]  bind_addr     要绑定的地址，可为 NULL。
 * @param[in]  port          要绑定的端口字符串。
 * @param[out] bindstr_out   用于返回实际绑定地址的字符串表示。
 * @param[in]  bindstr_len   `bindstr_out` 缓冲区长度。
 * @return 成功返回 0，失败返回负错误码。
 */
static int setup_socket(int *sock_out, const char *bind_addr, const char *port, char *bindstr_out, size_t bindstr_len) {
  int              err = 0;
  struct addrinfo *res = NULL, *rp, hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM, .ai_flags = AI_PASSIVE};
  int              sock = -1;

  int gai_err = getaddrinfo(bind_addr, port, &hints, &res);
  if (gai_err != 0) {
    log_error("getaddrinfo: %s", gai_strerror(gai_err));
    err = -EINVAL;
    goto err_cleanup;
  }

  /* 遍历候选地址，直到成功创建并绑定 socket。 */
  for (rp = res; rp; rp = rp->ai_next) {
    sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sock == -1)
      continue;

    if (bind(sock, rp->ai_addr, rp->ai_addrlen) == 0)
      break; // 成功绑定

    close(sock);
    sock = -1;
  }

  try2(sock != -1 ? 0 : -errno, "cannot create and bind socket: %s", strerror(errno));
  try2(addr_to_str((struct sockaddr_storage *) rp->ai_addr, bindstr_out, bindstr_len));

  /* 成功后把 socket 所有权交给调用方，避免在统一清理路径中被关闭。 */
  *sock_out = sock;
  sock      = -1;
  err       = 0;

err_cleanup:
  if (sock != -1)
    close(sock);
  if (res)
    freeaddrinfo(res);
  return err;
}

static bool detect_ktuctl(void) {
  const char *val   = getenv("TUTUICMPTUNNEL_USE_KTUCTL");
  uint32_t    val_n = 0;

  if (val && !parse_u32(val, &val_n) && val_n) {
    return true;
  }

  const char *modpath = "/sys/module/tutuicmptunnel";
  struct stat st;

  if (!stat(modpath, &st)) {
    return true;
  }

  return false;
}

static bool sudo_enabled(void) {
  const char *val   = getenv("TUTUICMPTUNNEL_DISABLE_SUDO");
  uint32_t    val_n = 0;

  if (val && !parse_u32(val, &val_n) && val_n) {
    return false;
  }

  return true;
}

/* 写全部数据，处理 short write 和 EINTR */
static int write_all(int fd, const void *buf, size_t len) {
  const uint8_t *p    = buf;
  size_t         left = len;
  while (left > 0) {
    ssize_t n = write(fd, p, left);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -errno;
    }
    if (n == 0)
      return -EPIPE;
    p += n;
    left -= (size_t) n;
  }
  return 0;
}

static int set_cloexec(int fd) {
  int flags = fcntl(fd, F_GETFD);

  if (flags < 0)
    return -1;

  return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/* 父进程标准 fd 被关闭时 pipe() 可能返回 0/1/2，adddup2(fd, fd) 会退化；
 * 把管道端提升到 >=3 并加 FD_CLOEXEC，避免依赖具体 libc 对等 fd 的处理。
 * F_DUPFD_CLOEXEC 自 Linux 2.6.24 起可用，musl/glibc 均提供该 fcntl 命令。 */
static int prepare_pipe_end(int *fd) {
  int newfd;

  if (*fd < STDERR_FILENO + 1) {
    newfd = fcntl(*fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (newfd < 0)
      return -1;
    close(*fd);
    *fd = newfd;
    return 0;
  }

  return set_cloexec(*fd);
}

/**
 * @brief Executes a command in a child process, capturing stdout/stderr.
 * @param[out] resp_buf      Buffer to store the command's output.
 * @param[out] resp_len_out  Pointer to store the length of the output.
 * @param[in]  cmd           The command script to execute.
 * @return 0 on success, non-zero on failure.
 */
static int execute_command(char *resp_buf, size_t *resp_len_out, size_t resp_buf_size, const uint8_t *cmd, size_t cmd_len) {
  int                        err        = 0;
  int                        inpipe[2]  = {-1, -1};
  int                        outpipe[2] = {-1, -1};
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t          attr;
  sigset_t                   sigdef;
  pid_t                      pid = -1;
  int                        status;
  char                      *argv[8];
  size_t                     argc       = 0;
  bool                       fa_ready   = false;
  bool                       attr_ready = false;
  bool                       sudo       = sudo_enabled();
  const char                *tuctl_prog = "tuctl";

  if (detect_ktuctl()) {
    log_info("Use ktuctl instead of tuctl");
    tuctl_prog = "ktuctl";
  }

  /* 管道端提升到 >=3 且带 FD_CLOEXEC。POSIX 规定 file action 按加入顺序执行，
   * 之后才关闭仍带 FD_CLOEXEC 的 fd：adddup2 先复制到 0/1/2（新 fd 清除
   * FD_CLOEXEC），原始管道端随后在 exec 时自动关闭，因此无需 addclose。 */
  try2_e(pipe(inpipe), "pipe: %s", strerrno);
  try2_e(prepare_pipe_end(&inpipe[0]), "prepare inpipe[0] failed: %s", strerrno);
  try2_e(prepare_pipe_end(&inpipe[1]), "prepare inpipe[1] failed: %s", strerrno);
  try2_e(pipe(outpipe), "pipe: %s", strerrno);
  try2_e(prepare_pipe_end(&outpipe[0]), "prepare outpipe[0] failed: %s", strerrno);
  try2_e(prepare_pipe_end(&outpipe[1]), "prepare outpipe[1] failed: %s", strerrno);

  /* posix_spawn 系列失败返回正错误码，取负匹配 try2 的 <0 约定 */
  try2(-posix_spawn_file_actions_init(&fa), "posix_spawn_file_actions_init failed: %s", strret);
  fa_ready = true;
  try2(-posix_spawn_file_actions_adddup2(&fa, inpipe[0], STDIN_FILENO), "adddup2 stdin failed: %s", strret);
  try2(-posix_spawn_file_actions_adddup2(&fa, outpipe[1], STDOUT_FILENO), "adddup2 stdout failed: %s", strret);
  try2(-posix_spawn_file_actions_adddup2(&fa, outpipe[1], STDERR_FILENO), "adddup2 stderr failed: %s", strret);

  try2(-posix_spawnattr_init(&attr), "posix_spawnattr_init failed: %s", strret);
  attr_ready = true;
  sigemptyset(&sigdef);
  sigaddset(&sigdef, SIGPIPE);
  /* 父进程忽略 SIGPIPE 时，exec 不会重置被忽略的信号；用 SETSIGDEF 恢复子进程默认行为 */
  try2(-posix_spawnattr_setsigdefault(&attr, &sigdef), "setsigdefault failed: %s", strret);
  try2(-posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF), "setflags failed: %s", strret);

  if (sudo)
    argv[argc++] = "sudo";
  argv[argc++] = (char *) tuctl_prog;
  argv[argc++] = "script";
  argv[argc++] = "-";
  argv[argc]   = NULL;

  /* 传入 environ，使子进程继承父进程环境（本函数无需改动环境变量） */
  try2_ret(-posix_spawnp(&pid, argv[0], &fa, &attr, argv, environ), -1, "posix_spawnp %s failed: %s", argv[0], strret);

  close(inpipe[0]);
  inpipe[0] = -1;
  close(outpipe[1]);
  outpipe[1] = -1;

  try2(write_all(inpipe[1], cmd, cmd_len));
  try2_e(close(inpipe[1])); // Close pipe to signal EOF to child
  inpipe[1] = -1;

  *resp_len_out = 0;
  ssize_t n;
  while ((n = read(outpipe[0], resp_buf + *resp_len_out, resp_buf_size - *resp_len_out)) > 0) {
    *resp_len_out += n;
    if (*resp_len_out >= resp_buf_size)
      break;
  }
  try2_e(close(outpipe[0]));
  outpipe[0] = -1;

  for (;;) {
    pid_t waited = waitpid(pid, &status, 0);
    if (waited == pid)
      break;
    if (waited < 0 && errno != EINTR) {
      err = -1;
      goto err_cleanup;
    }
  }
  pid = -1; // 已回收，避免 err_cleanup 重复 waitpid

  if (!WIFEXITED(status)) {
    log_error("command terminated abnormally (signal %d)", WTERMSIG(status));
    err = -ECHILD;
    goto err_cleanup;
  }

  if (WEXITSTATUS(status) != 0) {
    log_warn("command exited with status %d, but response has %zu bytes", WEXITSTATUS(status), *resp_len_out);
    /* 如果已经有输出（可能是错误信息），仍然尝试发送给客户端 */
    if (*resp_len_out == 0) {
      log_error("command failed with no output");
      err = -EIO;
      goto err_cleanup;
    }
    /* 否则继续，让上层发送已读取的错误信息 */
  }

  err = 0;

err_cleanup:
  // Ensure all pipe fds are closed in parent on error
  if (inpipe[0] != -1)
    close(inpipe[0]);
  if (inpipe[1] != -1)
    close(inpipe[1]);
  if (outpipe[0] != -1)
    close(outpipe[0]);
  if (outpipe[1] != -1)
    close(outpipe[1]);
  /* spawn 成功后若后续读写失败，回收子进程，避免遗留僵尸（或仍在运行的子进程）。
   * 先关掉父端 pipe：子进程若阻塞在写 stdout，会在默认 SIGPIPE 处置下退出。 */
  if (pid > 0) {
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
      ;
  }
  if (fa_ready)
    posix_spawn_file_actions_destroy(&fa);
  if (attr_ready)
    posix_spawnattr_destroy(&attr);
  return err;
}

/**
 * @brief 替换命令中的所有 @client_ip@ 占位符为客户端源地址，结果写入新分配的缓冲区。
 *
 * 设计：输出使用独立缓冲区，失败时不影响原始命令，调用者负责 free(*out)。
 *
 * 安全保证：
 * - *out和*out_len 在参数验证后立即初始化为 NULL/0
 * - 全路径拒绝 cmd_len == SIZE_MAX，防止 cmd_len + 1 和 SIZE_MAX - 1 - cmd_len 下溢
 * - 无占位符时直接返回原始副本，不调用 getnameinfo()
 * - 地址族仅接受 AF_INET / AF_INET6，其他返回 -EAFNOSUPPORT
 * - 验证 cli_len 上下界
 * - 增长/缩短分别计算，无无符号下溢
 * - 失败时 *out 为 NULL，不会泄漏
 * - 构造完成后验证 w - buf == replaced_len
 *
 * 注意：UDP 源地址可被伪造，不代表已认证的客户端身份。
 *
 * @pre cmd 指向至少 cmd_len 字节的可读内存。
 * @pre 若 cmd 中包含 @client_ip@，则 cli 指向有效的
 *      sockaddr_storage，且 cli_len 与地址族匹配并且不超过 sizeof(*cli)。
 *
 * @param[in]  cmd      原始命令。
 * @param[in]  cmd_len  命令长度，必须 < SIZE_MAX。
 * @param[in]  cli      recvfrom() 返回的客户端地址（可为 NULL，仅无占位符时允许）。
 * @param[in]  cli_len  recvfrom() 返回的地址结构实际长度。
 * @param[out] out      替换后的命令（调用者 free），失败时为 NULL。
 * @param[out] out_len  替换后的命令长度。
 * @return 0 成功，-EINVAL 参数错误，-EAFNOSUPPORT 不支持的地址族，
 *         -ENOMEM 内存不足。
 */
static int replace_client_ip(const uint8_t *cmd, size_t cmd_len, const struct sockaddr_storage *cli, socklen_t cli_len,
                             uint8_t **out, size_t *out_len) {
  static const char placeholder[]   = "@client_ip@";
  const size_t      placeholder_len = sizeof(placeholder) - 1;
  int               err             = 0;
  int               gai_err;
  char              ip_str[NI_MAXHOST];
  size_t            ip_len = 0;
  size_t            count  = 0;
  size_t            new_len, replaced_len;
  uint8_t          *buf = NULL;
  const uint8_t    *r;
  uint8_t          *w;

  /* 参数验证 */
  if (!cmd || !out || !out_len) {
    err = -EINVAL;
    goto err_cleanup;
  }

  *out     = NULL;
  *out_len = 0;

  /* 全路径拒绝 SIZE_MAX，防止后续 cmd_len + 1 和 SIZE_MAX - 1 - cmd_len 下溢 */
  if (cmd_len == SIZE_MAX) {
    err = -ENOMEM;
    goto err_cleanup;
  }

  /* 先扫描占位符数量 */
  r = cmd;
  while ((r = memmem(r, (size_t) (cmd + cmd_len - r), placeholder, placeholder_len)) != NULL) {
    count++;
    r += placeholder_len;
  }

  /* 无占位符直接复制返回，不需要 cli */
  if (count == 0) {
    buf = try2_p(malloc(cmd_len + 1));
    memcpy(buf, cmd, cmd_len);
    buf[cmd_len] = '\0';
    *out         = buf;
    *out_len     = cmd_len;
    buf          = NULL; /* 所有权转移给调用者，防止 err_cleanup 释放 */
    err_cleanup(0);
  }

  /* 有占位符，需要 cli */
  if (!cli) {
    err_cleanup(-EINVAL);
  }

  /* 验证 cli_len 上下界 */
  if (cli_len > sizeof(*cli)) {
    err_cleanup(-EINVAL);
  }

  switch (cli->ss_family) {
  case AF_INET:
    if (cli_len < sizeof(struct sockaddr_in)) {
      err_cleanup(-EINVAL);
    }
    break;
  case AF_INET6:
    if (cli_len < sizeof(struct sockaddr_in6)) {
      err_cleanup(-EINVAL);
    }
    break;
  default:
    log_error("unsupported address family: %d", cli->ss_family);
    err_cleanup(-EAFNOSUPPORT);
  }

  gai_err = getnameinfo((const struct sockaddr *) cli, cli_len, ip_str, sizeof(ip_str), NULL, 0, NI_NUMERICHOST);
  if (gai_err != 0) {
    log_error("getnameinfo failed: %s", gai_strerror(gai_err));
    err_cleanup(-EINVAL);
  }
  ip_len = strlen(ip_str);

  /* 预计算替换后长度，按增长/缩短分支避免无符号下溢 */
  if (ip_len >= placeholder_len) {
    size_t growth = ip_len - placeholder_len;
    if (growth != 0 && count > (SIZE_MAX - 1 - cmd_len) / growth) {
      log_error("replacement length overflow");
      err_cleanup(-ENOMEM);
    }
    replaced_len = cmd_len + (count * growth);
  } else {
    size_t shrink = placeholder_len - ip_len;
    /* count * shrink <= cmd_len：每个被替换项至少占 placeholder_len 字节 */
    replaced_len = cmd_len - (count * shrink);
  }

  new_len = replaced_len + 1;

  buf = try2_p(malloc(new_len));

  /* 逐段复制并替换 */
  r = cmd;
  w = buf;
  while (1) {
    const uint8_t *pos = memmem(r, (size_t) (cmd + cmd_len - r), placeholder, placeholder_len);
    if (!pos) {
      memcpy(w, r, (size_t) (cmd + cmd_len - r));
      w += cmd + cmd_len - r;
      break;
    }
    memcpy(w, r, (size_t) (pos - r));
    w += pos - r;
    memcpy(w, ip_str, ip_len);
    w += ip_len;
    r = pos + placeholder_len;
  }

  if ((size_t) (w - buf) != replaced_len) {
    err = -EINVAL;
    goto err_cleanup;
  }

  *w       = '\0';
  *out     = buf;
  *out_len = (size_t) (w - buf);
  buf      = NULL; /* 所有权转移给调用者，防止 err_cleanup 释放 */

err_cleanup:
  free(buf); /* NULL 安全 */
  return err;
}

int main(int argc, char **argv) {
  const char *bind_addr  = "::";
  const char *port       = STR(DEFAULT_SERVER_PORT);
  char       *psk        = NULL;
  uint32_t    window     = DEFAULT_WINDOW;
  uint32_t    replay_max = DEFAULT_REPLAY_MAX;
  int         err        = 0;
  int         sock       = -1;

  struct replay_window rwin;
  int                  rwin_inited = 0;

  setup_pwhash_memlimit();

#ifdef USE_SODIUM
  try2(sodium_init(), "libsodium init failed: %s", "unknown error");
#endif
  try2(parse_arguments(argc, argv, &bind_addr, &port, &psk, &window, &replay_max));

  {
    char bindstr[128];
    try2(setup_socket(&sock, bind_addr, port, bindstr, sizeof(bindstr)));
    log_info("Server listen %s, replay window = %ds, max=%d", bindstr, window, replay_max);
  }

  replay_window_init(&rwin, window, replay_max);
  rwin_inited = 1;

  rate_limiter_t rl;
  rl_init(&rl);

  // Main processing loop
  while (1) {
    struct sockaddr_storage cli;
    socklen_t               clen = sizeof(cli);
    uint8_t                 buf[MAX_CT_SIZE], pt[MAX_PT_SIZE];
    unsigned long long      pt_len = 0;

    ssize_t len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *) &cli, &clen);
    if (len < 0) {
      perror("recvfrom");
      continue;
    }

    // 先按来源限速，超过速率直接丢包
    if (!rl_allow(&rl, &cli)) {
      char abuf[128];
      if (addr_to_str(&cli, abuf, sizeof(abuf)) == 0) {
        log_info("too many requests from %s, dropping", abuf);
      }
      continue;
    }

    if (decrypt_and_validate_packet(pt, &pt_len, buf, len, &rwin, psk, &cli) != 0) {
      continue;
    }

    if (remove_padding(pt, &pt_len)) {
      continue;
    }

    {
      char abuf[128];
      if (addr_to_str(&cli, abuf, sizeof(abuf)) == 0) {
        log_info("command from %s (%llu bytes)", abuf, pt_len);
        log_info("  %.*s", (int) pt_len, pt);
      }
    }

    {
      uint8_t *cmd     = pt;
      size_t   cmd_len = pt_len;
      uint8_t *new_cmd = NULL;
      char     resp[MAX_PT_SIZE];
      size_t   resp_len = 0;

      /* 替换 @client_ip@ 占位符 */
      if (replace_client_ip(pt, pt_len, &cli, clen, &new_cmd, &cmd_len) != 0) {
        log_error("client ip replacement failed");
        continue;
      }
      cmd = new_cmd;

      if (execute_command(resp, &resp_len, sizeof(resp), cmd, cmd_len) != 0) {
        log_error("command execution failed");
        free(new_cmd);
        continue;
      }
      free(new_cmd);

      /* Add random padding to response to obscure its length */
      {
        size_t padding_len;
        if (resp_len < sizeof(resp) - 2) {
          padding_len = tucrypto_randombytes_uniform(256);
          if (resp_len + padding_len >= sizeof(resp)) {
            padding_len = sizeof(resp) - resp_len - 1;
          }
          memset(resp + resp_len, '#', padding_len);
          resp_len += padding_len;
        }

        if (resp_len >= sizeof(resp)) {
          resp_len = sizeof(resp) - 1;
        }
      }

      resp[resp_len] = '\0';
      log_info("response: %zu bytes", resp_len);

      if (encrypt_and_send_packet(sock, (struct sockaddr *) &cli, clen, &rwin, psk, resp, resp_len, NULL)) {
        log_error("failed to send response");
      }
    }
  }

err_cleanup:
  if (rwin_inited)
    replay_window_free(&rwin);
  if (sock != -1)
    close(sock);
  free(psk);
  return err;
}

// vim: set sw=2 ts=2 expandtab:
