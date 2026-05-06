/*
 * util.c — PPAP user-space helpers awaiting a POSIX home, plus the
 * POSIX shims that wrap kernel-internal syscalls (sleep, fork, wait,
 * getuid/gid, execl, getpass).
 *
 * Each `uc_`-prefixed entry is a TODO marker — slated to move to a
 * standard header and lose the prefix when the matching POSIX surface
 * lands:
 *
 *   uc_copy_fd            stays as PPAP extension
 *   uc_parse_u32          future strtoul()-based replacement
 */

#include "lib/uclib.h"
#include "syscall.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ── Numeric parsing ──────────────────────────────────────────────── */

int uc_parse_u32(const char *s, uint32_t *out) {
  uint32_t v = 0;
  int base = 10;
  int seen = 0;

  if (!s || !*s) return -1;

  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    s += 2;
  }

  while (*s) {
    char c = *s++;
    uint32_t d;
    if (c >= '0' && c <= '9')
      d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f')
      d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      d = (uint32_t)(c - 'A' + 10);
    else
      return -1;
    if (d >= (uint32_t)base) return -1;
    v = v * (uint32_t)base + d;
    seen = 1;
  }

  if (!seen) return -1;
  *out = v;
  return 0;
}

/* ── File copy ───────────────────────────────────────────────────── */

long uc_copy_fd(int src_fd, int dst_fd) {
  /* 512 B matches a sector on UFS / vfat, which happens to be the
   * smallest "natural" chunk all the FS drivers are tuned for.  On
   * pcxt we also have a tight user stack, so keep it small. */
  char buf[512];
  long total = 0;
  for (;;) {
    ssize_t n = read(src_fd, buf, sizeof(buf));
    if (n == 0) return total;
    if (n < 0) return -1;
    ssize_t w = 0;
    while (w < n) {
      ssize_t m = write(dst_fd, buf + w, (size_t)(n - w));
      if (m <= 0) return -1;
      w += m;
    }
    total += (long)n;
  }
}

/* ── POSIX shims over the existing syscalls ───────────────────────── */

/* sleep() — POSIX returns seconds remaining if interrupted; we ignore
 * that detail and always report 0 (full sleep). */
unsigned sleep(unsigned seconds) {
  long ts[2] = {(long)seconds, 0};
  nanosleep(ts, (void *)0);
  return 0;
}

/* fork() — PPAP only has vfork semantics.  Rogue uses it for a single
 * shell-escape execve; vfork-then-exec is correct there. */
pid_t fork(void) { return vfork(); }

/* wait() — wait for any child. */
pid_t wait(int *status) { return waitpid(-1, status, 0); }

/* PPAP is single-user; no real uid/gid concept. */
uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }

/* execl(path, arg0, arg1, ..., NULL) — variadic wrapper over execve. */
int execl(const char *path, const char *arg0, ...) {
  /* Walk the va_args once to count, then again to build argv. */
  va_list ap;
  va_start(ap, arg0);
  int argc = 1; /* arg0 */
  while (va_arg(ap, const char *)) argc++;
  va_end(ap);

  /* On-stack argv with bounded size — caps execl at 31 args to keep
   * the user stack tight on Cortex-M0+. */
  if (argc > 31) return -1;
  const char *argv_local[32];
  argv_local[0] = arg0;
  va_start(ap, arg0);
  for (int i = 1; i < argc; i++)
    argv_local[i] = va_arg(ap, const char *);
  va_end(ap);
  argv_local[argc] = (void *)0;

  return execve(path, (char *const *)argv_local, environ);
}

/* getpass() — read a line from the controlling tty without echo.  No
 * TTY-mode toggling on PPAP yet, so this is a plain getline that
 * trims the trailing newline.  Returns a pointer into a static
 * buffer; caller copies if it must outlive the next call. */
char *getpass(const char *prompt) {
  static char buf[128];
  if (prompt && *prompt) fputs(prompt, stderr);
  if (!fgets(buf, (int)sizeof(buf), stdin)) return (void *)0;
  size_t n = strlen(buf);
  if (n && buf[n - 1] == '\n') buf[n - 1] = '\0';
  return buf;
}

/* ── Termios wrappers ─────────────────────────────────────────────── *
 *
 * Thin shims over the existing TCGETS / TCSETS ioctls.  TCSADRAIN and
 * TCSAFLUSH map to the same TCSETSW / TCSETSF on Linux; we accept the
 * action value and pick the matching ioctl. */

#include <termios.h>

int tcgetattr(int fd, struct termios *t) {
  return ioctl(fd, TCGETS, t);
}

int tcsetattr(int fd, int actions, const struct termios *t) {
  unsigned long req = TCSETS;
  if (actions == TCSADRAIN) req = TCSETSW;
  else if (actions == TCSAFLUSH) req = TCSETSF;
  return ioctl(fd, req, (void *)t);
}
