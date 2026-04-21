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
  uc_eputs("rmdir: failed to remove '");
  uc_eputs(path);
  uc_eputs("'\n");
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
    if (uc_strcmp(argv[argi], "--help") == 0) {
      uc_puts(
          "Usage: rmdir [-p] DIR...\n"
          "  -p  Also remove empty parent directories\n");
      return 0;
    }
    if (uc_strcmp(argv[argi], "-p") == 0) {
      opt_p = 1;
      argi++;
      continue;
    }
    uc_eputs("rmdir: unknown option: ");
    uc_eputs(argv[argi]);
    uc_eputs("\n");
    return 1;
  }

  if (argi >= argc) {
    uc_eputs("rmdir: missing operand\n");
    return 1;
  }

  int rc = 0;
  for (int i = argi; i < argc; i++) {
    if (remove_one(argv[i])) rc = 1;
  }
  return rc;
}
