//
// Created by Andrea Cocito on 02/11/25.
//

#pragma once
#include <libwebsockets.h>

#ifdef __cplusplus
extern "C" {
#endif

  /* Collect ?arg=... items from the request URL.
   * - Returns a NULL-terminated argv array (heap-allocated with strdup).
   * - *argc_out is set to the number of args (not counting NULL).
   * - Returns NULL / sets *argc_out=0 when there are no args.
   */
  char **ttyd_collect_url_args(struct lws *wsi, int *argc_out);

  /* Free argv returned by ttyd_collect_url_args() */
  void ttyd_free_argv(char **argv);

#ifdef __cplusplus
}
#endif
