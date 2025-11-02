//
// Created by Andrea Cocito on 02/11/25.
//

#include <stdlib.h>
#include <string.h>

#include "urlargs.h"

/* libwebsockets’ simple in-place URL decoder */
static int urldecode_inplace(char *s){
  size_t n = strlen(s);
  int in_len = (int)strlen(s);
  int out_len = lws_urldecode(s, s, in_len);  // dst, src, len
  if (out_len < 0) return -1;
  s[out_len] = '\0';
  s[n] = '\0';
  return 0;
}

char **ttyd_collect_url_args(struct lws *wsi, int *argc_out) {
  if (argc_out) *argc_out = 0;

  /* Upper bound: small fixed cap to avoid unbounded malloc churn */
  enum { MAX_ARGS = 128 };
  char *tmp[MAX_ARGS];
  int cnt = 0;

  char frag[512];
  int idx = 0;
  while (lws_hdr_copy_fragment(wsi, frag, (int)sizeof(frag), WSI_TOKEN_HTTP_URI_ARGS, idx++) > 0) {
    char *eq = strchr(frag, '=');
    if (!eq) continue;
    *eq++ = '\0';
    if (strcmp(frag, "arg") != 0) continue;
    if (urldecode_inplace(eq) != 0) continue;
    if (cnt < MAX_ARGS - 1) tmp[cnt++] = strdup(eq);
  }

  if (cnt == 0) return NULL;

  char **argv = (char **)calloc((size_t)cnt + 1, sizeof(char *));
  for (int i = 0; i < cnt; ++i) argv[i] = tmp[i];
  argv[cnt] = NULL;

  if (argc_out) *argc_out = cnt;
  return argv;
}

void ttyd_free_argv(char **argv) {
  if (!argv) return;
  for (char **p = argv; *p; ++p) free(*p);
  free(argv);
}
