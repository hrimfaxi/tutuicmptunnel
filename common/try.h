#pragma once

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#ifdef BPF
#define log_error TUTU_LOG
#endif

#define redecl(_type, _name, _off, _ctx, _ret, _method)                                                                        \
  _name = ({                                                                                                                   \
    _type *_ptr = (void *) (__u64) (_ctx)->data + (_off);                                                                      \
    if (unlikely((__u64) _ptr + sizeof(_type) > (__u64) _method(_ctx)))                                                        \
      return _ret;                                                                                                             \
    _ptr;                                                                                                                      \
  })
#define redecl_skb(_type, _name, _off, _ctx, _ret) redecl(_type, _name, _off, _ctx, _ret, skb_data_end)
#define redecl_xdp(_type, _name, _off, _ctx, _ret) redecl(_type, _name, _off, _ctx, _ret, xdp_data_end)

#define redecl_ok(type, name, off, skb)   redecl_skb(type, name, off, skb, TC_ACT_OK)
#define redecl_shot(type, name, off, skb) redecl_skb(type, name, off, skb, TC_ACT_SHOT)
#define redecl_pass(type, name, off, xdp) redecl_xdp(type, name, off, xdp, XDP_PASS)
#define redecl_drop(type, name, off, xdp) redecl_xdp(type, name, off, xdp, XDP_DROP)

#define decl_skb(type, name, off, ctx, ret) type *redecl_skb(type, name, off, ctx, ret)
#define decl_xdp(type, name, off, ctx, ret) type *redecl_xdp(type, name, off, ctx, ret)
#define decl_ok(type, name, off, skb)       decl_skb(type, name, off, skb, TC_ACT_OK)
#define decl_shot(type, name, off, skb)     decl_skb(type, name, off, skb, TC_ACT_SHOT)
#define decl_pass(type, name, off, xdp)     decl_xdp(type, name, off, xdp, XDP_PASS)
#define decl_drop(type, name, off, xdp)     decl_xdp(type, name, off, xdp, XDP_DROP)

#define _get_macro(_0, _1, _2, _3, _4, _5, NAME, ...) NAME

// Returns _ret while printing error.
#define ret(...)  _get_macro(_0, ##__VA_ARGS__, _ret_fmt, _ret_fmt, _ret_fmt, _ret_fmt, _ret, )(__VA_ARGS__)
#define _ret(ret) return (ret)
#define _ret_fmt(ret, ...)                                                                                                     \
  ({                                                                                                                           \
    log_error(__VA_ARGS__);                                                                                                    \
    return (ret);                                                                                                              \
  })

// Jumps to `err_cleanup`, returning _ret while printing error.
//
// Requires `err_cleanup` label, `err` to be defined inside function scope, and `err` to be
// returned after cleanup.
#define err_cleanup(...)                                                                                                       \
  _get_macro(_0, ##__VA_ARGS__, _cleanup_fmt, _cleanup_fmt, _cleanup_fmt, _cleanup_fmt, _cleanup, )(__VA_ARGS__)
#define _cleanup(ret)                                                                                                          \
  ({                                                                                                                           \
    err = (ret);                                                                                                               \
    goto err_cleanup;                                                                                                          \
  })
#define _cleanup_fmt(ret, ...)                                                                                                 \
  ({                                                                                                                           \
    log_error(__VA_ARGS__);                                                                                                    \
    err = (ret);                                                                                                               \
    goto err_cleanup;                                                                                                          \
  })

#define _get_macro(_0, _1, _2, _3, _4, _5, NAME, ...) NAME

// Tests int return value from a function. Used for functions that returns non-zero error.
#define try(expr, ...)                                                                                                         \
  ({                                                                                                                           \
    long _ret = (expr);                                                                                                        \
    if (unlikely(_ret < 0))                                                                                                    \
      ret(_ret, ##__VA_ARGS__);                                                                                                \
    _ret;                                                                                                                      \
  })

// Same as `try` with one arguments, but runs XDP subroutine
#define try_tc(expr)                                                                                                           \
  ({                                                                                                                           \
    long _ret = (expr);                                                                                                        \
    if (unlikely(_ret != TC_ACT_OK))                                                                                           \
      return _ret;                                                                                                             \
    _ret;                                                                                                                      \
  })

// Same as `try` with one arguments, but runs XDP subroutine
#define try_xdp(expr)                                                                                                          \
  ({                                                                                                                           \
    long _ret = (expr);                                                                                                        \
    if (unlikely(_ret != XDP_PASS))                                                                                            \
      return _ret;                                                                                                             \
    _ret;                                                                                                                      \
  })

// `try` but `err_cleanup`.
#define try2(expr, ...)                                                                                                        \
  ({                                                                                                                           \
    long _ret = (expr);                                                                                                        \
    if (unlikely(_ret < 0))                                                                                                    \
      err_cleanup(_ret, ##__VA_ARGS__);                                                                                        \
    _ret;                                                                                                                      \
  })

// `errno` is not available in BPF
#ifndef _BPF

// Same as `try`, but returns -errno
#define try_e(expr, ...)                                                                                                       \
  ({                                                                                                                           \
    long _ret = (expr);                                                                                                        \
    if (unlikely(_ret < 0)) {                                                                                                  \
      _ret = -errno;                                                                                                           \
      ret(-errno, ##__VA_ARGS__);                                                                                              \
    }                                                                                                                          \
    _ret;                                                                                                                      \
  })

// `try_e` but `err_cleanup`.
#define try2_e(expr, ...)                                                                                                      \
  ({                                                                                                                           \
    long _ret = (expr);                                                                                                        \
    if (unlikely(_ret < 0)) {                                                                                                  \
      _ret = -errno;                                                                                                           \
      err_cleanup(_ret, ##__VA_ARGS__);                                                                                        \
    }                                                                                                                          \
    _ret;                                                                                                                      \
  })

// Similar to `try_e`, but for function that returns a pointer.
#define try_p(expr, ...)                                                                                                       \
  ({                                                                                                                           \
    void *_ptr = (expr);                                                                                                       \
    if (unlikely(!_ptr)) {                                                                                                     \
      long _ret = -errno;                                                                                                      \
      ret(_ret, ##__VA_ARGS__);                                                                                                \
    }                                                                                                                          \
    _ptr;                                                                                                                      \
  })

// Similar to `try2_e`, but for function that returns a pointer.
#define try2_p(expr, ...)                                                                                                      \
  ({                                                                                                                           \
    void *_ptr = (expr);                                                                                                       \
    if (unlikely(!_ptr)) {                                                                                                     \
      long _ret = -errno;                                                                                                      \
      err_cleanup(_ret, ##__VA_ARGS__);                                                                                        \
    }                                                                                                                          \
    _ptr;                                                                                                                      \
  })

#endif // _BPF

// Tests int return value from a function, but return a different value when failed.
#define try_ret(expr, ret)                                                                                                     \
  ({                                                                                                                           \
    int _val = (expr);                                                                                                         \
    if (unlikely(_val < 0))                                                                                                    \
      return ret;                                                                                                              \
    _val;                                                                                                                      \
  })

#define try_ok(x)   try_ret(x, TC_ACT_OK)
#define try_shot(x) try_ret(x, TC_ACT_SHOT)
#define try_pass(x) try_ret(x, XDP_PASS)
#define try_drop(x) try_ret(x, XDP_DROP)

#define try2_ret(expr, retval, ...)                                                                                            \
  ({                                                                                                                           \
    long _ret = (expr);                                                                                                        \
    if (unlikely(_ret < 0))                                                                                                    \
      err_cleanup(retval, ##__VA_ARGS__);                                                                                      \
    _ret;                                                                                                                      \
  })

#define try2_p_ret(expr, retval, ...)                                                                                          \
  ({                                                                                                                           \
    void *_ptr = (expr);                                                                                                       \
    if (unlikely(!_ptr))                                                                                                       \
      err_cleanup(retval, ##__VA_ARGS__);                                                                                      \
    _ptr;                                                                                                                      \
  })

#define try2_ok(x, ...)   try2_ret(x, TC_ACT_OK, ##__VA_ARGS__)
#define try2_shot(x, ...) try2_ret(x, TC_ACT_SHOT, ##__VA_ARGS__)

#define try2_p_ok(x, ...)   try2_p_ret(x, TC_ACT_OK, ##__VA_ARGS__)
#define try2_p_shot(x, ...) try2_p_ret(x, TC_ACT_SHOT, ##__VA_ARGS__)

#define try2_pass(x, ...) try2_ret(x, XDP_PASS, ##__VA_ARGS__)
#define try2_drop(x, ...) try2_ret(x, XDP_DROP, ##__VA_ARGS__)

#define try2_p_pass(x, ...) try2_p_ret(x, XDP_PASS, ##__VA_ARGS__)
#define try2_p_drop(x, ...) try2_p_ret(x, XDP_DROP, ##__VA_ARGS__)

// Tests pointer return value from a function, but return a different value when failed.
#define try_p_ret(expr, ret)                                                                                                   \
  ({                                                                                                                           \
    void *_ptr = (expr);                                                                                                       \
    if (unlikely(!_ptr))                                                                                                       \
      return ret;                                                                                                              \
    _ptr;                                                                                                                      \
  })

#define try_p_ok(x)   try_p_ret(x, TC_ACT_OK)
#define try_p_shot(x) try_p_ret(x, TC_ACT_SHOT)
#define try_p_pass(x) try_p_ret(x, XDP_PASS)
#define try_p_drop(x) try_p_ret(x, XDP_DROP)

#define strret   strerror(-_ret)
#define strerrno strerror(errno)

#define ARRAY_SIZE(n) (sizeof(n) / sizeof(n[0]))

// vim: set sw=2 ts=2 expandtab:
