//
// Created by Andrea Cocito on 02/11/25.
//

#include <errno.h>

#include "wspipe.h"
// ReSharper disable once CppUnusedIncludeDirective
#include <fcntl.h>
#include <limits.h>  // INT_MAX
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <strings.h>

#include "runcmd.h"
#include "server.h"
#include "urlargs.h"

static int g_log_stderr = 0;
void set_errlog(int enable) { g_log_stderr = enable ? 1 : 0; }

#define WS_MAX_CHUNK 32768
#define SUL_POLL_USEC 1000

/* Build a temporary argv vector that borrows pointers from base + extra */
static char **merge_argv(const char *const *base, char *const *extra, int extra_count) {
  int basec = 0;
  while (base[basec]) basec++;

  char **out = (char **)malloc(((size_t)basec + (size_t)extra_count + 1) * sizeof(char *));
  if (!out) return NULL;

  for (int i = 0; i < basec; ++i) out[i] = (char *)base[i];        // borrow
  for (int j = 0; j < extra_count; ++j) out[basec + j] = extra[j]; // borrow
  out[basec + extra_count] = NULL;
  return out;
}

static void nb_set(int fd) {
  if (fd < 0) return;
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  (void)fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
}

static int spawn_pipes(const char *const *argv, pid_t *out_pid, int *fd_in_w, int *fd_out_r, int *fd_err_r) {
  int in_p[2], out_p[2], err_p[2];

#if defined(O_CLOEXEC) && (defined(__linux__) || defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || defined(__OpenBSD__))
  if (pipe2(in_p,  O_CLOEXEC) ||
      pipe2(out_p, O_CLOEXEC) ||
      pipe2(err_p, O_CLOEXEC))
    return -1;
#else
  if (pipe(in_p) || pipe(out_p) || pipe(err_p))
    return -1;
#endif

  pid_t pid = fork();
  if (pid < 0) return -1;

  if (pid == 0) {
    /* child */
    if (server) {
      if (server->cwd) (void)chdir(server->cwd);
    }
    (void)dup2(in_p[0], 0);
    (void)dup2(out_p[1], 1);
    (void)dup2(err_p[1], 2);
    close(in_p[1]);
    close(out_p[0]);
    close(err_p[0]);
    close(in_p[0]);
    close(out_p[1]);
    close(err_p[1]);

    if (argv && argv[0])
      execvp(argv[0], (char *const *)argv);
    _exit(127);
  }

  /* parent */
  close(in_p[0]);
  close(out_p[1]);
  close(err_p[1]);
  *out_pid = pid;
  *fd_in_w = in_p[1];
  *fd_out_r = out_p[0];
  *fd_err_r = err_p[0];
  nb_set(*fd_in_w);
  nb_set(*fd_out_r);
  nb_set(*fd_err_r);
  return 0;
}

/* ----- stderr → LWS logging (line buffered, consistent with ttyd) ----- */

static void flush_stderr_line(struct pss_raw *pss) {
  if (!pss->err_used) return;
  size_t used = pss->err_used;

  /* strip one trailing '\n' if present */
  if (used && pss->err_line[used - 1] == '\n') used--;

  if (used >= sizeof(pss->err_line)) used = sizeof(pss->err_line) - 1;
  pss->err_line[used] = '\0';

  /* child stderr is a warning in ttyd */
  lwsl_warn("[child:%d] %s\n", (int)pss->pid, pss->err_line);

  pss->err_used = 0;
}

static void accumulate_stderr(struct pss_raw *pss, const uint8_t *p, size_t n) {
  while (n--) {
    char c = (char)*p++;
    if (pss->err_used < sizeof(pss->err_line) - 1) {
      pss->err_line[pss->err_used++] = c;
    }
    if (c == '\n' || pss->err_used == sizeof(pss->err_line) - 1) {
      flush_stderr_line(pss);
    }
  }
}

/* ----- data pump helpers ----- */

static int pump_fd_to_wsbuf(struct pss_raw *pss, int fd) {
  if (pss->ws_len) return 0; /* wait for WRITEABLE to flush previous */
  unsigned char *p = (unsigned char *)malloc(LWS_PRE + WS_MAX_CHUNK);
  if (!p) return 0;
  ssize_t n = read(fd, p + LWS_PRE, WS_MAX_CHUNK);
  if (n > 0) {
    pss->ws_buf = p;
    pss->ws_len = (size_t)n;
    lws_callback_on_writable(pss->wsi);
    return 1;
  }
  free(p);
  return 0;
}

static void pump_err(struct pss_raw *pss) {
  uint8_t tmp[WS_MAX_CHUNK];
  ssize_t n = read(pss->fd_err_r, tmp, sizeof(tmp));
  if (n <= 0) return;

  if (g_log_stderr) {
    accumulate_stderr(pss, tmp, (size_t)n);
    return;
  }

  if (pss->ws_len) {
    // Already a frame queued: just drop silently (explicit)
    // Optional: lwsl_debug("stderr chunk dropped (%zd bytes)\n", n);
    return;
  }
  pss->ws_buf = (unsigned char *)malloc(LWS_PRE + (size_t)n);
  if (!pss->ws_buf) return;
  memcpy(pss->ws_buf + LWS_PRE, tmp, (size_t)n);
  pss->ws_len = (size_t)n;
  lws_callback_on_writable(pss->wsi);
}

static void sul_poll_cb(lws_sorted_usec_list_t *sul) {
  struct pss_raw *pss = lws_container_of(sul, struct pss_raw, sul);
  if (!pss || !pss->wsi) return;

  (void)pump_fd_to_wsbuf(pss, pss->fd_out_r);
  pump_err(pss);

  if (!pss->child_dead) {
    int st;
    for (;;) {
    pid_t r = waitpid(pss->pid, &st, WNOHANG);
      if (r == 0) break;                  // still running
    if (r == pss->pid) {
      pss->child_dead = 1;
      lws_close_reason(pss->wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
      lws_callback_on_writable(pss->wsi);
        break;
      }
      if (r < 0 && errno == EINTR) continue;
      break;
    }
  }
  lws_sul_schedule(lws_get_context(pss->wsi), 0, &pss->sul, sul_poll_cb, SUL_POLL_USEC);
}

/* ----- protocol callback ----- */

// ReSharper disable once CppParameterMayBeConstPtrOrRef
int callback_pipe(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
  struct pss_raw *pss = (struct pss_raw *)user;

  switch (reason) {
    case LWS_CALLBACK_ESTABLISHED: {
      memset(pss, 0, sizeof(*pss));
      pss->wsi = wsi;
      pss->fd_in_w = pss->fd_out_r = pss->fd_err_r = -1;

      if (server && server->url_arg) {
        pss->argv = ttyd_collect_url_args(wsi, &pss->argc);  // owns strings
      }

      const char *const *base = ttyd_runcmd();
      char **tmp_vec = NULL;   // borrowing vector; always free when allocated
      const char *const *argv_for_spawn = base;

      if (pss->argv) {
        tmp_vec = merge_argv(base, pss->argv, pss->argc);
        if (!tmp_vec) {
          ttyd_free_argv(pss->argv);
          pss->argv = NULL;
          pss->argc = 0;
          return -1;
        }
        argv_for_spawn = (const char *const *)tmp_vec;
      }

      int rc = spawn_pipes(argv_for_spawn, &pss->pid, &pss->fd_in_w, &pss->fd_out_r, &pss->fd_err_r);

      if (tmp_vec) {
        free(tmp_vec);   // free the vector itself; strings are owned by base / pss->argv
        tmp_vec = NULL;
      }

      if (rc < 0) {
        if (pss->argv) {
          ttyd_free_argv(pss->argv);
          pss->argv = NULL;
          pss->argc = 0;
        }
        return -1;
      }

      lws_sul_schedule(lws_get_context(wsi), 0, &pss->sul, sul_poll_cb, SUL_POLL_USEC);
      lws_callback_on_writable(wsi);
      server->client_count++;
      break;
    }

    case LWS_CALLBACK_RECEIVE:
      /* Honor --writable: if not set, silently ignore client input (read-only). */
      if (!server || !server->writable) {
        break;
      }

      if (pss->fd_in_w >= 0 && len) {
        const uint8_t *p = (const uint8_t *)in;
        size_t left = len;
        while (left) {
          ssize_t w = write(pss->fd_in_w, p, (ssize_t)left);
          if (w > 0) { p += (size_t)w; left -= (size_t)w; continue; }
          if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break; /* try later */
          /* EPIPE or fatal: stop consuming further */
          break;
        }
      }
      break;

    case LWS_CALLBACK_SERVER_WRITEABLE:
      if (pss->ws_len && pss->ws_buf) {
        (void)lws_write(wsi, pss->ws_buf + LWS_PRE, (int)pss->ws_len, LWS_WRITE_BINARY);
        free(pss->ws_buf);
        pss->ws_buf = NULL;
        pss->ws_len = 0;
      }
      if (pss->child_dead && !pss->ws_len) return -1;
      break;

    case LWS_CALLBACK_CLOSED:
    case LWS_CALLBACK_WSI_DESTROY:
      lws_sul_cancel(&pss->sul);
      if (pss->fd_in_w  >= 0) close(pss->fd_in_w);
      if (pss->fd_out_r >= 0) close(pss->fd_out_r);
      if (pss->fd_err_r >= 0) close(pss->fd_err_r);
      if (pss->pid > 0) {
        kill(pss->pid, SIGHUP);
        kill(pss->pid, SIGTERM);
        pss->pid = -1;
      }
      if (pss->argv) {
        ttyd_free_argv(pss->argv);
        pss->argv = NULL;
        pss->argc = 0;
      }
      if (g_log_stderr && pss->err_used) {
        flush_stderr_line(pss);
      }
      if (pss->ws_buf) {
        free(pss->ws_buf);
        pss->ws_buf = NULL;
        pss->ws_len = 0;
      }
      if (server->client_count > 0)
        server->client_count--;

      if ((server->once || server->exit_no_conn) && server->client_count == 0) {
        lwsl_notice("exiting due to the --once/--exit-no-conn option.\n");
        force_exit = true;
        lws_cancel_service(context);
        exit(0);
      }
      break;

    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION: {
      if (server->once && server->client_count > 0) return 1;
      if (server->max_clients > 0 && server->client_count >= server->max_clients) return 1;
      if (server->check_origin) {
        char origin[256]={0}, host[256]={0};
        if (lws_hdr_copy(wsi, origin, sizeof(origin), WSI_TOKEN_ORIGIN) <= 0 ||
            lws_hdr_copy(wsi, host, sizeof(host), WSI_TOKEN_HOST) <= 0)
          return 1;

        const char *prot,*addr,*path; int port;
        if (lws_parse_uri(origin, &prot, &addr, &port, &path)) return 1;

        char expect[256];
        if (port == 80 || port == 443) snprintf(expect, sizeof(expect), "%s", addr);
        else snprintf(expect, sizeof(expect), "%s:%d", addr, port);

        if (strcasecmp(expect, host) != 0) return 1;
      }

      if (server->auth_header) {
        char auth_user[128];
        size_t name_sz = strlen(server->auth_header);
        if (name_sz > (size_t)INT_MAX)
          return 1;  // absurd header name

        int rc = lws_hdr_custom_copy(
            wsi,
            auth_user,
            (int)sizeof(auth_user),
            server->auth_header,
            (int)name_sz
        );
        if (rc <= 0)
          return 1;
      } else if (server->credential) {
        char hdr[256];
        int n = lws_hdr_copy(wsi, hdr, (int)sizeof(hdr), WSI_TOKEN_HTTP_AUTHORIZATION);
        if (n < 7 || strncmp(hdr, "Basic ", 6) != 0)
          return 1;
        if (strcmp(hdr + 6, server->credential) != 0)
          return 1;
      }
      return 0;
    }

    default:
      break;
  }
  return 0;
}
