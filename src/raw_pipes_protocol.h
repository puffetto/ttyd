#pragma once
#include <stddef.h>       /* size_t */
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
};

void rawpipes_set_log_stderr(int enable);

int callback_ttyd_raw_pipes(struct lws *wsi,
                            enum lws_callback_reasons reason,
                            void *user, void *in, size_t len);

