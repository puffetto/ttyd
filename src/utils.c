#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp, strncasecmp */

#if defined(__linux__) && !defined(__ANDROID__)
const char *sys_signame[NSIG] = {
    "zero", "HUP",  "INT",  "QUIT", "ILL",    "TRAP",   "ABRT",  "UNUSED", "FPE",  "KILL", "USR1",
    "SEGV", "USR2", "PIPE", "ALRM", "TERM",   "STKFLT", "CHLD",  "CONT",   "STOP", "TSTP", "TTIN",
    "TTOU", "URG",  "XCPU", "XFSZ", "VTALRM", "PROF",   "WINCH", "IO",     "PWR",  "SYS",  NULL};
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#undef NSIG
#define NSIG 33
const char *sys_signame[NSIG] = {
    "zero", "HUP", "INT",  "QUIT", "ILL",    "TRAP", "IOT",   "EMT",  "FPE",  "KILL", "BUS",
    "SEGV", "SYS", "PIPE", "ALRM", "TERM",   "URG",  "STOP",  "TSTP", "CONT", "CHLD", "TTIN",
    "TTOU", "IO",  "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "PWR",  "USR1", "USR2", NULL};
#endif

void *xmalloc(size_t size) {
  if (size == 0) return NULL;
  void *p = malloc(size);
  if (!p) abort();
  return p;
}

void *xrealloc(void *ptr, size_t size) {
  if (size == 0) {          /* free-on-zero size, return NULL */
    free(ptr);
    return NULL;
}
  void *np = realloc(ptr, size);  /* don’t clobber ptr on failure */
  if (!np) abort();
  return np;
}
char *uppercase(char *s) {
  if (!s) return NULL;
  char *orig = s;
  while(*s) {
    *s = (char)toupper((unsigned char)*s);
    ++s;
  }
  return orig;
}

char *lowercase(char *s) {
  if (!s) return NULL;
  char *orig = s;
  while(*s) {
    *s = (char)tolower((unsigned char)*s);
    ++s;
  }
  return orig;
}

bool endswith(const char *str, const char *suffix) {
  if (!str || !suffix) return false;
  size_t str_len = strlen(str);
  size_t suffix_len = strlen(suffix);
  if (suffix_len == 0) return true;
  if (str_len < suffix_len) return false;
  return memcmp(str + (str_len - suffix_len), suffix, suffix_len) == 0;
}

int get_sig_name(int sig, char *buf, size_t len) {
  const char *name = (sig > 0 && sig < NSIG) ? sys_signame[sig] : NULL;
  if (!name) name = "unknown";
  int n = snprintf(buf, len, "SIG%s", name);
  (void)uppercase(buf);
  return n;
}

int get_sig(const char *sig_name) {
  if (!sig_name) return -1;

  /* skip leading spaces */
  const char *s = sig_name;
  while (isspace((unsigned char)*s)) ++s;

  /* accept optional "SIG" prefix, case-insensitive */
  if (strncasecmp(s, "SIG", 3) == 0) s += 3;

  /* Try numeric (e.g. "15") */
  errno = 0;
  char *end = NULL;
  long val = strtol(s, &end, 10);
  if (errno == 0 && end && *end == '\0' && end != s && val > 0 && val < NSIG) {
    return (int)val;
  }

  /* Try named (e.g. "TERM") via sys_signame */
  for (int sig = 1; sig < NSIG; ++sig) {
    const char *name = sys_signame[sig];
    if (name != NULL && (strcasecmp(name, sig_name) == 0 || strcasecmp(name, sig_name + 3) == 0))
      return sig;
  }

  return -1; /* unknown */
}

int open_uri(char *uri) {
#ifdef __APPLE__
  char command[512];
  /* Minimal quoting: wrap in single quotes; escape existing single quotes */
  char quoted[384];
  size_t qi = 0;
  quoted[qi++] = '\'';
  for (const char *p = uri; *p && qi + 4 < sizeof(quoted); ++p) {
    if (*p == '\'') { memcpy(&quoted[qi], "'\\''", 4); qi += 4; }
    else            { quoted[qi++] = *p; }
  }
  quoted[qi++] = '\'';
  quoted[qi] = '\0';
  snprintf(command, sizeof(command), "open %s > /dev/null 2>&1", quoted);
  return system(command);
#elif defined(_WIN32) || defined(__CYGWIN__)
  return ShellExecute(0, 0, uri, 0, 0, SW_SHOW) > (HINSTANCE)32 ? 0 : 1;
#else
  // check if X server is running
  if (system("xset -q > /dev/null 2>&1")) return 1;
  char command[512];
  char quoted[384];
  size_t qi = 0;
  quoted[qi++] = '\'';
  for (const char *p = uri; *p && qi + 4 < sizeof(quoted); ++p) {
    if (*p == '\'') { memcpy(&quoted[qi], "'\\''", 4); qi += 4; }
    else            { quoted[qi++] = *p; }
  }
  quoted[qi++] = '\'';
  quoted[qi] = '\0';
  snprintf(command, sizeof(command), "xdg-open %s > /dev/null 2>&1", quoted);
  return system(command);
#endif
}

#ifdef _WIN32
char *strsep(char **sp, char *sep) {
  char *p, *s;
  if (sp == NULL || *sp == NULL || **sp == '\0') return NULL;
  s = *sp;
  p = s + strcspn(s, sep);
  if (*p != '\0') *p++ = '\0';
  *sp = p;
  return s;
}

const char *quote_arg(const char *arg) {
  int len = 0, n = 0;
  int force_quotes = 0;
  char *q, *d;
  const char *p = arg;
  if (!*p) force_quotes = 1;
  while (*p) {
    if (isspace((unsigned char)*p) || *p == '*' || *p == '?' || *p == '{' || *p == '\'')
      force_quotes = 1;
    else if (*p == '"')
      n++;
    else if (*p == '\\') {
      int count = 0;
      while (*p == '\\') {
        count++;
        p++;
        len++;
      }
      if (*p == '"' || !*p) n += count * 2 + 1;
      continue;
    }
    len++;
    p++;
  }
  if (!force_quotes && n == 0) return arg;

  d = q = xmalloc(len + n + 3);
  *d++ = '"';
  while (*arg) {
    if (*arg == '"')
      *d++ = '\\';
    else if (*arg == '\\') {
      int count = 0;
      while (*arg == '\\') {
        count++;
        *d++ = *arg++;
      }
      if (*arg == '"' || !*arg) {
        while (count-- > 0) *d++ = '\\';
        if (!*arg) break;
        *d++ = '\\';
      }
    }
    *d++ = *arg++;
  }
  *d++ = '"';
  *d++ = '\0';
  return q;
}

void print_error(char *func) {
  LPVOID buffer;
  DWORD dw = GetLastError();
  FormatMessage(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      NULL, dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR)&buffer, 0, NULL);
  wprintf(L"== %s failed with error %lu: %s", func, dw, (wchar_t*)buffer);
  LocalFree(buffer);
}
#endif
