#include "tuctl.h"

#include <bpf/libbpf.h>
#include <errno.h>
#include <linux/limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"
#include "try.h"

#include "tutuicmptunnel.debug.skel.h"
#include "tutuicmptunnel.skel.h"

static const char *map_names[] = {
  "config_map",   "user_map",      "egress_peer_map", "ingress_peer_map", "session_map", "build_type_map",
#ifndef DISABLE_BPF_TIMER
  "gc_timer_map", "gc_switch_map",
#endif
};

static const char *prog_names[] = {
  "handle_egress",
  "handle_ingress",
#ifndef DIABLE_BPF_TIMER
  "handle_gc_timer",
#endif
};

static int ensure_pin_dir(void) {
  struct stat st;
  if (stat(PIN_DIR, &st) == -1) {
    if (mkdir(PIN_DIR, 0755) && errno != EEXIST) {
      log_error("ensure_pin_dir failed: %s", strerror(errno));
      return -1;
    }
  }
  return 0;
}

typedef int (*get_prog_fn)(void *bpf, struct bpf_program **prog);
typedef struct bpf_map *(*find_map_fn)(void *bpf, const char *name);

static int auto_pin_common(void *bpf, get_prog_fn get_handle_egress, get_prog_fn get_handle_ingress,
                           get_prog_fn get_handle_gc_timer, find_map_fn find_map, const char **map_names, int map_count) {
  struct bpf_map     *map;
  int                 i, found = 0, err;
  struct bpf_program *prog;
  int                 map_fd = -1;

  (void) get_handle_gc_timer;

  try(ensure_pin_dir());
  // handle_egress
  try(get_handle_egress(bpf, &prog));
  try2(bpf_program__pin(prog, PIN_DIR "handle_egress"), _("bpf_program__pin %s: %s"), "egress", strret);

  // handle_ingress
  try(get_handle_ingress(bpf, &prog));
  try2(bpf_program__pin(prog, PIN_DIR "handle_ingress"), _("bpf_program__pin %s: %s"), "ingress", strret);

#ifndef DISABLE_BPF_TIMER
  if (get_handle_gc_timer) {
    // handle_gc_timer
    try(get_handle_gc_timer(bpf, &prog));
    try2(bpf_program__pin(prog, PIN_DIR "handle_gc_timer"), _("bpf_program__pin %s: %s"), "gc_timer", strret);
  }
#endif

  // maps
  for (i = 0; i < map_count; i++) {
    const char *name = map_names[i];
    map              = find_map(bpf, name);
    if (!map) {
      log_error("map %s not found in bpf object", name);
      continue;
    }

    map_fd = bpf_map__fd(map);
    if (map_fd < 0) {
      log_error("map %s invalid fd: %s", name, strerror(-map_fd));
      continue;
    }

    char pin_path[PATH_MAX];
    snprintf(pin_path, sizeof(pin_path), "%s%s", PIN_DIR, name);
    pin_path[sizeof(pin_path) - 1] = '\0';

    // Remove old pin if exists
    unlink(pin_path);

    int err = bpf_map__pin(map, pin_path);
    if (err) {
      log_error("failed to pin map %s to %s: %s", name, pin_path, strerror(-err));
      continue;
    }

    // printf("%s (fd %d) pinned to %s\n", name, map_fd, pin_path);
    found++;
  }

  err = found ? 0 : -ENOENT;
err_cleanup:
  if (map_fd >= 0)
    close(map_fd);
  return err;
}

static int get_handle_egress_normal(void *bpf, struct bpf_program **prog) {
  *prog = ((struct tutuicmptunnel *) bpf)->progs.handle_egress;
  return 0;
}

static int get_handle_ingress_normal(void *bpf, struct bpf_program **prog) {
  *prog = ((struct tutuicmptunnel *) bpf)->progs.handle_ingress;
  return 0;
}

#ifndef DISABLE_BPF_TIMER
static int get_handle_gc_timer_normal(void *bpf, struct bpf_program **prog) {
  *prog = ((struct tutuicmptunnel *) bpf)->progs.handle_gc_timer;
  return 0;
}
#endif

static struct bpf_map *find_map_normal(void *bpf, const char *name) {
  return bpf_object__find_map_by_name(((struct tutuicmptunnel *) bpf)->obj, name);
}

int auto_pin(struct tutuicmptunnel *bpf) {
  return auto_pin_common(bpf, get_handle_egress_normal, get_handle_ingress_normal,
#ifdef DISABLE_BPF_TIMER
                         NULL
#else
                         get_handle_gc_timer_normal
#endif
                         ,
                         find_map_normal, map_names, NELEMS(map_names));
}

static int get_handle_egress_debug(void *bpf, struct bpf_program **prog) {
  *prog = ((struct tutuicmptunnel_debug *) bpf)->progs.handle_egress;
  return 0;
}

static int get_handle_ingress_debug(void *bpf, struct bpf_program **prog) {
  *prog = ((struct tutuicmptunnel_debug *) bpf)->progs.handle_ingress;
  return 0;
}

#ifndef DISABLE_BPF_TIMER
static int get_handle_gc_timer_debug(void *bpf, struct bpf_program **prog) {
  *prog = ((struct tutuicmptunnel_debug *) bpf)->progs.handle_gc_timer;
  return 0;
}
#endif

static struct bpf_map *find_map_debug(void *bpf, const char *name) {
  return bpf_object__find_map_by_name(((struct tutuicmptunnel_debug *) bpf)->obj, name);
}

int auto_pin_debug(struct tutuicmptunnel_debug *bpf) {
  return auto_pin_common(bpf, get_handle_egress_debug, get_handle_ingress_debug,
#ifdef DISABLE_BPF_TIMER
                         NULL
#else
                         get_handle_gc_timer_debug
#endif
                         ,
                         find_map_debug, map_names, NELEMS(map_names));
}

int unauto_pin(void) {
  unsigned int i;
  char         pin_path[PATH_MAX];

  for (i = 0; i < NELEMS(map_names); i++) {
    const char *name = map_names[i];
    snprintf(pin_path, sizeof(pin_path), "%s%s", PIN_DIR, name);
    unlink(pin_path);
  }

  for (i = 0; i < NELEMS(prog_names); i++) {
    const char *name = prog_names[i];
    snprintf(pin_path, sizeof(pin_path), "%s%s", PIN_DIR, name);
    unlink(pin_path);
  }

  return rmdir(PIN_DIR);
}

// vim: set sw=2 ts=2 expandtab:
