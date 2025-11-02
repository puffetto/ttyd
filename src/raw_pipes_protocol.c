//
// Created by Andrea Cocito on 02/11/25.
//

#include "raw_pipes_protocol.h"
#include "launch_cmd.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libwebsockets.h>

static int g_log_stderr = 0;
void rawpipes_set_log_stderr(int enable) { g_log_stderr = enable ? 1 : 0; }

#define WS_MAX_CHUNK   32768
#define SUL_POLL_USEC  1000

static void nb_set(int fd) {
    if (fd < 0) return;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int spawn_pipes(pid_t *out_pid, int *fd_in_w, int *fd_out_r, int *fd_err_r) {
    int in_p[2], out_p[2], err_p[2];
    if (pipe(in_p) || pipe(out_p) || pipe(err_p)) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* child */
        dup2(in_p[0], 0);
        dup2(out_p[1], 1);
        dup2(err_p[1], 2);
        close(in_p[1]); close(out_p[0]); close(err_p[0]);
        close(in_p[0]); close(out_p[1]); close(err_p[1]);

        const char * const *argv = ttyd_launch_argv();
        execvp(argv[0], (char * const *)argv);
        _exit(127);
    }

    /* parent */
    close(in_p[0]);  close(out_p[1]);  close(err_p[1]);
    *out_pid  = pid;
    *fd_in_w  = in_p[1];
    *fd_out_r = out_p[0];
    *fd_err_r = err_p[0];
    nb_set(*fd_in_w); nb_set(*fd_out_r); nb_set(*fd_err_r);
    return 0;
}

static void stderr_prefix_write(pid_t pid, const char *buf, size_t n) {
    char prefix[64];
    int plen = snprintf(prefix, sizeof(prefix), "[child:%d] ", (int)pid);
    struct iovec iov[2] = {
        { .iov_base = prefix, .iov_len = (size_t)plen },
        { .iov_base = (void*)buf, .iov_len = n }
    };
    (void)writev(STDERR_FILENO, iov, 2);
}

static void stderr_line_accum(struct pss_raw *pss, const uint8_t *p, size_t n) {
    while (n--) {
        char c = (char)*p++;
        pss->err_line[pss->err_used++] = c;
        if (c == '\n' || pss->err_used == sizeof(pss->err_line)) {
            stderr_prefix_write(pss->pid, pss->err_line, pss->err_used);
            pss->err_used = 0;
        }
    }
}

static int pump_fd_to_wsbuf(struct pss_raw *pss, int fd) {
    if (pss->ws_len) return 0; /* wait for WRITEABLE to flush previous */
    unsigned char *p = malloc(LWS_PRE + WS_MAX_CHUNK);
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
        stderr_line_accum(pss, tmp, (size_t)n);
    } else {
        if (!pss->ws_len) {
            pss->ws_buf = malloc(LWS_PRE + (size_t)n);
            if (!pss->ws_buf) return;
            memcpy(pss->ws_buf + LWS_PRE, tmp, (size_t)n);
            pss->ws_len = (size_t)n;
            lws_callback_on_writable(pss->wsi);
        }
    }
}

static void sul_poll_cb(lws_sorted_usec_list_t *sul) {
    struct pss_raw *pss = lws_container_of(sul, struct pss_raw, sul);
    if (!pss || !pss->wsi) return;

    (void)pump_fd_to_wsbuf(pss, pss->fd_out_r);
    pump_err(pss);

    if (!pss->child_dead) {
        int st; pid_t r = waitpid(pss->pid, &st, WNOHANG);
        if (r == pss->pid) {
            pss->child_dead = 1;
            lws_close_reason(pss->wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
            lws_callback_on_writable(pss->wsi);
        }
    }
    lws_sul_schedule(lws_get_context(pss->wsi), 0, &pss->sul, sul_poll_cb, SUL_POLL_USEC);
}

int callback_ttyd_raw_pipes(struct lws *wsi,
                            enum lws_callback_reasons reason,
                            void *user, void *in, size_t len)
{
    struct pss_raw *pss = (struct pss_raw *)user;

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        memset(pss, 0, sizeof(*pss));
        pss->wsi = wsi;
        pss->fd_in_w = pss->fd_out_r = pss->fd_err_r = -1;
        if (spawn_pipes(&pss->pid, &pss->fd_in_w, &pss->fd_out_r, &pss->fd_err_r) < 0)
            return -1;
        lws_sul_schedule(lws_get_context(wsi), 0, &pss->sul, sul_poll_cb, SUL_POLL_USEC);
        lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_RECEIVE:
        if (pss->fd_in_w >= 0 && len) {
            const uint8_t *p = (const uint8_t*)in;
            (void)write(pss->fd_in_w, p, (ssize_t)len); /* immediate */
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
        if (g_log_stderr && pss->err_used) {
            stderr_prefix_write(pss->pid, pss->err_line, pss->err_used);
            pss->err_used = 0;
        }
        if (pss->ws_buf) { free(pss->ws_buf); pss->ws_buf = NULL; pss->ws_len = 0; }
        break;

    default:
        break;
    }
    return 0;
}
