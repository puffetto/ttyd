#pragma once
// ReSharper disable once CppUnusedIncludeDirective
#include <stddef.h>       /* size_t */
// ReSharper disable once CppUnusedIncludeDirective
#include <sys/types.h>    /* pid_t, u_char, etc (macOS needs this before lws) */
#include <libwebsockets.h>

struct pss_raw {
  struct lws *wsi;
  pid_t pid;
  int fd_in_w;
  int fd_out_r;
  int fd_err_r;
  unsigned char *ws_buf;
  size_t ws_len;
  char err_line[2048];
  size_t err_used;
  lws_sorted_usec_list_t sul;  /* libwebsockets micro-timer */
  int child_dead;
  char **argv;
  int argc;
};

void set_errlog(int enable);

int callback_pipe(struct lws *wsi,
                            enum lws_callback_reasons reason,
                            void *user, void *in, size_t len);

