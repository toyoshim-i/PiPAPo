/*
 * uname.c — print system information
 *
 * Usage: uname [-asnrvm] [--help]
 *   -s  sysname      (default if no flag)
 *   -n  nodename
 *   -r  release
 *   -v  version
 *   -m  machine
 *   -a  all of the above, in -snrvm order
 */

#include "lib/uclib.h"

#define F_S 0x01
#define F_N 0x02
#define F_R 0x04
#define F_V 0x08
#define F_M 0x10
#define F_A (F_S | F_N | F_R | F_V | F_M)

static void print_field(const char *field, int *first) {
  if (!*first) putchar(' ');
  uc_puts(field);
  *first = 0;
}

int main(int argc, char *argv[]) {
  int flags = 0;

  for (int argi = 1; argi < argc; argi++) {
    const char *a = argv[argi];
    if (strcmp(a, "--help") == 0) {
      uc_puts(
          "Usage: uname [-asnrvm]\n"
          "  -s  Kernel name (default)\n"
          "  -n  Node (host) name\n"
          "  -r  Kernel release\n"
          "  -v  Kernel version\n"
          "  -m  Machine (architecture) name\n"
          "  -a  All of the above\n");
      return 0;
    }
    if (a[0] != '-' || a[1] == '\0') {
      uc_eputs("uname: unknown argument: ");
      uc_eputs(a);
      uc_eputs("\n");
      return 1;
    }
    for (const char *p = a + 1; *p; p++) {
      switch (*p) {
        case 'a': flags |= F_A; break;
        case 's': flags |= F_S; break;
        case 'n': flags |= F_N; break;
        case 'r': flags |= F_R; break;
        case 'v': flags |= F_V; break;
        case 'm': flags |= F_M; break;
        default:
          uc_eputs("uname: unknown option: -");
          putchar(*p);
          uc_eputs("\n");
          return 1;
      }
    }
  }

  if (flags == 0) flags = F_S;

  struct utsname u;
  if (uname(&u) < 0) {
    uc_eputs("uname: syscall failed\n");
    return 1;
  }

  int first = 1;
  if (flags & F_S) print_field(u.sysname, &first);
  if (flags & F_N) print_field(u.nodename, &first);
  if (flags & F_R) print_field(u.release, &first);
  if (flags & F_V) print_field(u.version, &first);
  if (flags & F_M) print_field(u.machine, &first);
  putchar('\n');
  return 0;
}
