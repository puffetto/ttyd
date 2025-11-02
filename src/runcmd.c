//
// Created by Andrea Cocito on 02/11/25.
//
#include <stddef.h>

#include "runcmd.h"

static const char *default_shell[] = { "/bin/sh", "-c", "exec /bin/sh", NULL };
static const char * const *g_launch_argv = default_shell;

void ttyd_setargv(const char * const *argv) {
  g_launch_argv = (argv && argv[0]) ? argv : default_shell;
}

const char * const *ttyd_runcmd(void) {
  return g_launch_argv ? g_launch_argv : default_shell;
}
