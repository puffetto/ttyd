#pragma once

/* Returns the argv used for the normal TTY session (what ttyd would exec).
  * Never returns NULL; falls back to ttyd's default when none provided.
  * Lifetime: whole process (argv is owned by existing ttyd code). */
const char * const *ttyd_runcmd(void);

/* Call this once in server.c after you've built cmd_argv[]. */
void ttyd_setargv(const char * const *argv);
