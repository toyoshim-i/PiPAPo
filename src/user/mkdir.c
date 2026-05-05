/*
 * mkdir.c — create directories
 *
 * Usage: mkdir [-p] [-m MODE] DIR...
 *   -p       Create parent directories as needed; don't fail if the
 *            target already exists.
 *   -m MODE  Explicit octal mode (e.g. 0755, 700).
 *
 * Default mode is 0755.
 */

#include "common/errno.h"
#include "lib/uclib.h"

static int opt_p;
static int opt_mode = 0755;

static void err_path(const char *path) {
  uc_eputs("mkdir: cannot create directory: ");
  uc_eputs(path);
  uc_eputs("\n");
}

static int parse_octal_mode(const char *s, int *out) {
  int m = 0;
  if (!*s) return -1;
  while (*s) {
    if (*s < '0' || *s > '7') return -1;
    m = (m << 3) | (*s - '0');
    s++;
  }
  *out = m;
  return 0;
}

/* -p: stat each prefix; skip if it already exists, mkdir if not.
 * Accepting EEXIST alone is not enough: on PPAP a writable mount
 * (tmpfs at /tmp) can be rooted under a read-only mount (UFS root),
 * so the kernel returns EROFS before ever reaching the child mount. */
static int make_with_parents(char *path) {
  char *p = path;
  if (*p == '/') p++;
  while (*p) {
    while (*p && *p != '/') p++;
    char saved = *p;
    *p = '\0';
    int r = 0;
    if (access(path, 0) != 0) {
      r = mkdir(path, opt_mode);
      if (r < 0 && r == -(int)EEXIST) r = 0;
    }
    *p = saved;
    if (r < 0) {
      err_path(path);
      return 1;
    }
    if (*p) p++;
  }
  return 0;
}

static int make_one(char *path) {
  if (opt_p) return make_with_parents(path);
  if (mkdir(path, opt_mode) < 0) {
    err_path(path);
    return 1;
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
    if (strcmp(argv[argi], "--help") == 0) {
      uc_puts(
          "Usage: mkdir [-p] [-m MODE] DIR...\n"
          "  -p       Create parent directories as needed\n"
          "  -m MODE  Set mode (octal, default 0755)\n");
      return 0;
    }
    if (strcmp(argv[argi], "-p") == 0) {
      opt_p = 1;
      argi++;
      continue;
    }
    if (strcmp(argv[argi], "-m") == 0) {
      if (argi + 1 >= argc) {
        uc_eputs("mkdir: option -m requires an argument\n");
        return 1;
      }
      if (parse_octal_mode(argv[argi + 1], &opt_mode) < 0) {
        uc_eputs("mkdir: invalid mode: ");
        uc_eputs(argv[argi + 1]);
        uc_eputs("\n");
        return 1;
      }
      argi += 2;
      continue;
    }
    uc_eputs("mkdir: unknown option: ");
    uc_eputs(argv[argi]);
    uc_eputs("\n");
    return 1;
  }

  if (argi >= argc) {
    uc_eputs("mkdir: missing operand\n");
    return 1;
  }

  int rc = 0;
  for (int i = argi; i < argc; i++) {
    if (make_one(argv[i])) rc = 1;
  }
  return rc;
}
