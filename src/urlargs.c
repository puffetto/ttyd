#include "urlargs.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <libwebsockets.h>

/* percent-decode in place; returns length */
static size_t pct_decode(char *s) {
  char *r = s, *w = s;
  while (*r) {
    if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
      int hi = r[1] <= '9' ? r[1]-'0' : (tolower((unsigned char)r[1])-'a'+10);
      int lo = r[2] <= '9' ? r[2]-'0' : (tolower((unsigned char)r[2])-'a'+10);
      *w++ = (char)((hi<<4) | lo);
      r += 3;
    } else if (*r == '+') {
      *w++ = ' ';
      r++;
    } else {
      *w++ = *r++;
    }
  }
  *w = '\0';
  return (size_t)(w - s);
}

/* Collect ?arg=... values into a NULL-terminated argv.
 * Returns NULL and sets *argc_out=0 if none.
 */
char **ttyd_collect_url_args(struct lws *wsi, int *argc_out) {
  if (argc_out) *argc_out = 0;

  /* Iterate fragments of the query string */
  int frag = 0;
  char kv[512];

  /* First pass: count */
  int count = 0;
  for (;;) {
    int n = lws_hdr_copy_fragment(wsi, kv, sizeof(kv), WSI_TOKEN_HTTP_URI_ARGS, frag++);
    if (n <= 0) break; /* no more */
    /* kv looks like "key=value" or just "key" */
    char *eq = memchr(kv, '=', (size_t)n);
    size_t keylen = eq ? (size_t)(eq - kv) : (size_t)n;
    if (keylen == 3 && strncmp(kv, "arg", 3) == 0) {
      count++;
    }
  }
  if (count == 0) return NULL;

  /* Second pass: extract values */
  char **argv = (char **)calloc((size_t)count + 1, sizeof(char *));
  if (!argv) return NULL;

  frag = 0;
  int idx = 0;
  for (;;) {
    int n = lws_hdr_copy_fragment(wsi, kv, sizeof(kv), WSI_TOKEN_HTTP_URI_ARGS, frag++);
    if (n <= 0) break;
    char *eq = memchr(kv, '=', (size_t)n);
    size_t keylen = eq ? (size_t)(eq - kv) : (size_t)n;
    if (!(keylen == 3 && strncmp(kv, "arg", 3) == 0))
      continue;

    const char *val = eq ? (eq + 1) : "";
    /* duplicate and percent-decode */
    char *dup = strdup(val ? val : "");
    if (!dup) { /* clean up on OOM */
      for (int i = 0; i < idx; ++i) free(argv[i]);
      free(argv);
      return NULL;
    }
    pct_decode(dup);
    argv[idx++] = dup;
  }

  if (argc_out) *argc_out = idx;
  return argv;
}

void ttyd_free_argv(char **argv) {
  if (!argv) return;
  for (char **p = argv; *p; ++p) free(*p);
  free(argv);
}
