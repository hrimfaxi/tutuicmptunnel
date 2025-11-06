#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <netdb.h>
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
#include "parser.h"
#include "try.h"

// 覆盖argv里的psk
static void wipe_argv_psk(int argc, char **argv, const char *psk) {
  for (int i = 0; i < argc; ++i) {
    if (argv[i] && strstr(argv[i], psk)) {
      memset(argv[i], '*', strlen(argv[i]));
    }
  }
}

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
 * @brief Creates and binds a UDP socket.
 * @param[out] sock_out      Pointer to store the resulting socket file descriptor.
 * @param[out] bindstr_out   Buffer to store the string representation of the bound address.
 * @return 0 on success, non-zero on failure.
 */
static int setup_socket(int *sock_out, const char *bind_addr, const char *port, char *bindstr_out, size_t bindstr_len) {
  int              err = 0;
  struct addrinfo *res, *rp, hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM, .ai_flags = AI_PASSIVE};
  int              sock = -1;

  try2(getaddrinfo(bind_addr, port, &hints, &res), "getaddrinfo: %s", gai_strerror(_ret));

  for (rp = res; rp; rp = rp->ai_next) {
    sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sock != -1) {
      if (bind(sock, rp->ai_addr, rp->ai_addrlen) == 0)
        break; // Success
      close(sock);
      sock = -1;
    }
  }

  try2(sock != -1 ? 0 : -errno, "cannot create and bind socket: %s", strerror(errno));
  try2(addr_to_str((struct sockaddr_storage *) rp->ai_addr, bindstr_out, bindstr_len));

  *sock_out = sock;
  freeaddrinfo(res);
  return 0;

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

/**
 * @brief Executes a command in a child process, capturing stdout/stderr.
 * @param[out] resp_buf      Buffer to store the command's output.
 * @param[out] resp_len_out  Pointer to store the length of the output.
 * @param[in]  cmd           The command script to execute.
 * @return 0 on success, non-zero on failure.
 */
static int execute_command(char *resp_buf, size_t *resp_len_out, size_t resp_buf_size, const uint8_t *cmd, size_t cmd_len) {
  int   err        = 0;
  int   inpipe[2]  = {-1, -1};
  int   outpipe[2] = {-1, -1};
  pid_t pid;
  bool  sudo = sudo_enabled();

  const char *tuctl_prog = "tuctl";

  if (detect_ktuctl()) {
    log_info("Use ktuctl instead of tuctl");
    tuctl_prog = "ktuctl";
  }

  try2(pipe(inpipe), "pipe: %s", strerror(errno));
  try2(pipe(outpipe), "pipe: %s", strerror(errno));

  pid = fork();
  if (pid == -1) {
    try2(-1, "fork: %s", strerror(errno));
  } else if (pid == 0) {
    // --- Child Process ---
    close(inpipe[1]);   // Close write end of inpipe
    dup2(inpipe[0], 0); // Redirect stdin from inpipe
    close(inpipe[0]);

    close(outpipe[0]);   // Close read end of outpipe
    dup2(outpipe[1], 1); // Redirect stdout to outpipe
    dup2(outpipe[1], 2); // Redirect stderr to outpipe
    close(outpipe[1]);

    if (sudo) {
      execlp("sudo", "sudo", tuctl_prog, "script", "-", NULL);
    } else {
      execlp(tuctl_prog, tuctl_prog, "script", "-", NULL);
    }

    perror("execlp"); // Should not be reached
    if (sudo) {
      log_error("sudo enabled, please check sudo setting");
    }
    exit(127);
  } else {
    // --- Parent Process ---
    try2(close(inpipe[0]));  // Close read end
    try2(close(outpipe[1])); // Close write end

    try2(write(inpipe[1], cmd, cmd_len) != -1 ? 0 : -errno);
    try2(close(inpipe[1])); // Close pipe to signal EOF to child

    *resp_len_out = 0;
    ssize_t n;
    while ((n = read(outpipe[0], resp_buf + *resp_len_out, resp_buf_size - *resp_len_out)) > 0) {
      *resp_len_out += n;
      if (*resp_len_out >= resp_buf_size)
        break;
    }
    try2(close(outpipe[0]));
    try2(waitpid(pid, NULL, 0));
    err = 0;
  }

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
  wipe_argv_psk(argc, argv, psk);

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

    char   resp[MAX_PT_SIZE];
    size_t resp_len = 0;
    if (execute_command(resp, &resp_len, sizeof(resp), pt, pt_len) != 0) {
      log_error("command execution failed");
      continue;
    }

    // Add random padding to response to obscure its length
    size_t padding_len;
    if (resp_len < sizeof(resp) - 2) { // Need space for padding and null terminator
      padding_len = tucrypto_randombytes_uniform(256);
      if (resp_len + padding_len >= sizeof(resp)) {
        padding_len = sizeof(resp) - resp_len - 1;
      }
      memset(resp + resp_len, '#', padding_len);
      resp_len += padding_len;
    }
    resp[resp_len] = '\0'; // Ensure it's a null-terminated string for logging
    log_info("response: %zu bytes", resp_len);

    if (encrypt_and_send_packet(sock, (struct sockaddr *) &cli, clen, &rwin, psk, resp, resp_len, NULL)) {
      log_error("failed to send response");
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

// vim: set sw=2 expandtab :
