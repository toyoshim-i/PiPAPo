/*
 * touch.c — stamp file mtime, creating the file if needed
 *
 * Usage: touch [-c] [-a] [-m] file...
 *   -c  Do not create files that don't exist (POSIX "no-create" mode).
 *   -a  Accepted for POSIX compatibility — has no effect because
 *       utimes sets both atime and mtime together.
 *   -m  Same as -a (accepted, no effect).
 *
 * With no flags, an existing file's atime and mtime are updated to
 * the current wallclock; a missing file is created (empty, mode 0644)
 * and its mtime is stamped by the create path.
 *
 * Not yet supported: -t STAMP (set a specific timestamp), -r REF
 * (copy stamps from a reference file).  Reach for busybox `touch`
 * until those land here.
 */

#include "lib/uclib.h"

static void print_err(const char *msg, const char *path) {
  uc_eputs("touch: ");
  uc_eputs(msg);
  uc_eputs(": ");
  uc_eputs(path);
  uc_eputs("\n");
}

static int touch_one(const char *path, int no_create) {
  struct stat st;
  if (stat(path, &st) == 0) {
    /* Exists: stamp atime + mtime to now.  Kernel sets ctime = now
     * on every .utimes regardless of times pointer. */
    if (utimes(path, 0) < 0) {
      print_err("cannot stamp", path);
      return 1;
    }
    return 0;
  }
  if (no_create) return 0;
  int fd = open(path, O_WRONLY | O_CREAT, 0644);
  if (fd < 0) {
    print_err("cannot create", path);
    return 1;
  }
  close(fd);
  /* A fresh create already stamps mtime = ctime = now via the FS
   * driver's .create op (tmpfs_create, ufs_create).  No explicit
   * utimes is needed here — it would be a redundant write. */
  return 0;
}

int main(int argc, char *argv[]) {
  int argi = 1;
  int no_create = 0;

  while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
    if (strcmp(argv[argi], "--help") == 0) {
      uc_puts(
          "Usage: touch [-c] [-a] [-m] file...\n"
          "  -c  Do not create files that don't exist\n"
          "  -a  Accepted for POSIX compat (no effect — atime and\n"
          "      mtime are stamped together)\n"
          "  -m  Same as -a\n");
      return 0;
    }
    const char *p = argv[argi] + 1;
    while (*p) {
      switch (*p) {
        case 'c':
          no_create = 1;
          break;
        case 'a':
        case 'm':
          /* accepted for compat */
          break;
        default:
          uc_eputs("touch: unknown option: -");
          putchar(*p);
          uc_eputs("\n");
          return 1;
      }
      p++;
    }
    argi++;
  }

  if (argi >= argc) {
    uc_eputs("touch: missing file operand\n");
    return 1;
  }

  int rc = 0;
  for (; argi < argc; argi++) {
    if (touch_one(argv[argi], no_create)) rc = 1;
  }
  return rc;
}
