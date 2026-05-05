/*
 * rmdir.c — remove empty directories
 *
 * Usage: rmdir [-p] DIR...
 *   -p  After removing DIR, also remove its empty parent components
 *       (walk upward, rmdir each, stop on first error).
 *
 * Only empty directories are removed; the kernel returns -ENOTEMPTY
 * (or -EEXIST on some filesystems) for non-empty targets.
 */

#include "lib/uclib.h"

static int opt_p;

static void err_path(const char *path) {
  fputs("rmdir: failed to remove '", stderr);
  fputs(path, stderr);
  fputs("'\n", stderr);
}

/* Truncate path at the last '/' (writes '\0' in place).  Returns 1 if
 * a further parent component exists to remove, 0 otherwise. */
static int chop_parent(char *path) {
  int i = 0;
  while (path[i]) i++;
  while (i > 0 && path[i] != '/') i--;
  /* Trailing slashes: chew past them so the next chop actually
   * truncates a name, not a bare separator. */
  while (i > 0 && path[i] == '/') {
    path[i] = '\0';
    i--;
  }
  if (i == 0) return 0; /* no more parents above root */
  return 1;
}

static int remove_one(char *path) {
  if (rmdir(path) < 0) {
    err_path(path);
    return 1;
  }
  if (!opt_p) return 0;
  while (chop_parent(path)) {
    if (rmdir(path) < 0) break; /* POSIX: stop silently on parent fail */
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
    if (strcmp(argv[argi], "--help") == 0) {
      fputs(
          "Usage: rmdir [-p] DIR...\n"
          "  -p  Also remove empty parent directories\n", stdout);
      return 0;
    }
    if (strcmp(argv[argi], "-p") == 0) {
      opt_p = 1;
      argi++;
      continue;
    }
    fputs("rmdir: unknown option: ", stderr);
    fputs(argv[argi], stderr);
    fputs("\n", stderr);
    return 1;
  }

  if (argi >= argc) {
    fputs("rmdir: missing operand\n", stderr);
    return 1;
  }

  int rc = 0;
  for (int i = argi; i < argc; i++) {
    if (remove_one(argv[i])) rc = 1;
  }
  return rc;
}
