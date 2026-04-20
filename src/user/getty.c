/*
 * getty.c — Minimal getty for PPAP
 *
 * Combined getty + login for a single-user embedded system.
 * Called from inittab:
 *   ttyS0::respawn:/bin/getty ttyS0
 *   tty1::respawn:/bin/getty -l tty1    (-l = auto-login, skip prompt)
 *
 * Flow:
 *   1. setsid()         — become session leader
 *   2. open /dev/<tty>  — as stdin, dup to stdout/stderr
 *   3. TIOCSCTTY        — acquire controlling terminal
 *   4. Unless -l: print prompt and wait for Enter
 *   5. execve /bin/sh
 */

#include "syscall.h"

#include "common/errno.h"

#define O_RDWR 2
#define TIOCSCTTY 0x540Eu

static void puts_fd(int fd, const char *s) {
  const char *p = s;
  while (*p) p++;
  write(fd, s, (size_t)(p - s));
}

/* Simple string append: returns pointer past the copied bytes */
static char *str_append(char *dst, const char *src) {
  while (*src) *dst++ = *src++;
  return dst;
}

static int streq(const char *a, const char *b) {
  while (*a && *b && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

int main(int argc, char **argv) {
  int auto_login = 0;
  const char *tty_name = "ttyS0";

  /* Parse args: [-l] <tty> */
  for (int i = 1; i < argc; i++) {
    if (streq(argv[i], "-l"))
      auto_login = 1;
    else
      tty_name = argv[i];
  }

  /* 1. New session */
  setsid();

  /* 2. Build "/dev/<tty>" path and open as stdin/stdout/stderr */
  char path[32];
  char *p = str_append(path, "/dev/");
  p = str_append(p, tty_name);
  *p = '\0';

  close(0);
  close(1);
  close(2);

  int fd = open(path, O_RDWR, 0);
  if (fd < 0) {
    /* Fallback: try without /dev/ prefix (already a full path?) */
    fd = open(tty_name, O_RDWR, 0);
  }
  if (fd < 0) _exit(1);

  /* Ensure fd 0, and dup to 1 and 2 */
  if (fd != 0) {
    dup2(fd, 0);
    close(fd);
  }
  dup2(0, 1);
  dup2(0, 2);

  /* 3. Acquire controlling terminal */
  ioctl(0, TIOCSCTTY, (void *)1);

  /* 4. Prompt (skipped with -l) */
  if (!auto_login) {
    puts_fd(1, "\nPress Enter to activate this console. ");
    char c;
    for (;;) {
      ssize_t n = read(0, &c, 1);
      if (n < 0 && -n == EINTR) continue;
      if (n <= 0) _exit(1);
      if (c == '\n' || c == '\r') break;
    }
    puts_fd(1, "\n");
  }

  /* 5. Exec shell as a login shell. */
  char *sh_argv[] = {"/bin/sh", "-l", (char *)0};
  char *sh_envp[] = {(char *)0};
  execve("/bin/sh", sh_argv, sh_envp);

  puts_fd(2, "getty: exec /bin/sh failed\n");
  _exit(1);
}
