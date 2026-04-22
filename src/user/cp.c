/*
 * cp.c — copy a single file
 *
 * Usage: cp [-f] SOURCE DEST
 *   -f  Force: if DEST exists, truncate it; default is O_CREAT|O_EXCL.
 *
 * DEST is treated as a filename, never a directory — "cp foo dir/"
 * (classic single-arg-to-directory sugar) is not supported yet.  When
 * that matters, reach for busybox cp.  Likewise `-r` / multiple
 * sources are deferred; this is the 90% case of the 90% case.
 */

#include "lib/uclib.h"

static int use_color = 1;
#define C(seq) (use_color ? (seq) : "")
#define C_RST C("\033[0m")
#define C_RED C("\033[31m")

static void die(const char *prefix, const char *path) {
  uc_eputs(C_RED);
  uc_eputs("cp: ");
  uc_eputs(C_RST);
  uc_eputs(prefix);
  uc_eputs(path);
  uc_eputs("\n");
  _exit(1);
}

int main(int argc, char *argv[]) {
  int argi = 1;
  int force = 0;

  while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
    if (uc_strcmp(argv[argi], "--help") == 0) {
      uc_puts(
          "Usage: cp [-f] [--no-color] SOURCE DEST\n"
          "  -f  Truncate DEST if it exists (default: fail if DEST exists)\n");
      return 0;
    }
    if (uc_strcmp(argv[argi], "--no-color") == 0) {
      use_color = 0;
      argi++;
      continue;
    }
    const char *p = argv[argi] + 1;
    while (*p) {
      switch (*p) {
        case 'f':
          force = 1;
          break;
        default:
          uc_eputs("cp: unknown option: -");
          uc_putc(*p);
          uc_eputs("\n");
          return 1;
      }
      p++;
    }
    argi++;
  }

  if (argi + 2 != argc) {
    uc_eputs("cp: expected SOURCE and DEST\n");
    return 1;
  }

  const char *src_path = argv[argi];
  const char *dst_path = argv[argi + 1];

  /* PPAP has no O_EXCL today, so guard the "destination exists" case
   * with an explicit stat.  Race-prone in a general OS, but PPAP is
   * single-user cooperatively-vfork'd — no one else is racing us. */
  if (!force) {
    struct stat st;
    if (stat(dst_path, &st) == 0) die("destination exists: ", dst_path);
  }

  int src_fd = open(src_path, O_RDONLY, 0);
  if (src_fd < 0) die("cannot open: ", src_path);

  int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (dst_fd < 0) {
    close(src_fd);
    die("cannot create: ", dst_path);
  }

  long n = uc_copy_fd(src_fd, dst_fd);
  close(src_fd);
  close(dst_fd);
  if (n < 0) die("copy failed: ", dst_path);
  return 0;
}
