/*
 * rm.c — remove files and (with -r) directory trees
 *
 * Usage: rm [-rRf] FILE...
 *   -r, -R  Recursively remove directories and their contents.
 *   -f      Ignore nonexistent files; never fail silently in POSIX
 *           style ("if the file does not exist, rm shall not write a
 *           diagnostic message and shall not modify the exit status").
 *
 * Keeps one struct dirent and one path buffer per recursion level on
 * the stack, so recursion depth is bounded by user-stack capacity
 * (~4 KB on PPAP) divided by ~200 B per frame.
 */

#include "common/dirent.h"
#include "common/fcntl.h"
#include "common/stat.h"
#include "lib/uclib.h"

#define RM_PATH_MAX 128

static int opt_f;
static int opt_r;

static void err_path(const char *path) {
  if (opt_f) return;
  fputs("rm: cannot remove '", stderr);
  fputs(path, stderr);
  fputs("'\n", stderr);
}

static int path_join(char *dst, const char *dir, const char *name) {
  int i = 0;
  while (dir[i] && i < RM_PATH_MAX - 2) {
    dst[i] = dir[i];
    i++;
  }
  if (i == 0 || dst[i - 1] != '/') dst[i++] = '/';
  int j = 0;
  while (name[j] && i < RM_PATH_MAX - 1) dst[i++] = name[j++];
  dst[i] = '\0';
  return name[j] == '\0' ? 0 : -1;
}

static int remove_any(const char *path);

static int remove_dir_contents(const char *path) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) return 1;
  int err = 0;
  struct dirent de;
  for (;;) {
    int n = getdents(fd, &de, sizeof(de));
    if (n <= 0) break;
    if (de.d_name[0] == '.' &&
        (de.d_name[1] == '\0' ||
         (de.d_name[1] == '.' && de.d_name[2] == '\0')))
      continue;
    char child[RM_PATH_MAX];
    if (path_join(child, path, de.d_name) < 0) {
      fputs("rm: path too long\n", stderr);
      err = 1;
      continue;
    }
    if (remove_any(child)) err = 1;
  }
  close(fd);
  return err;
}

static int remove_any(const char *path) {
  struct stat st;
  if (stat(path, &st) < 0) {
    err_path(path);
    return opt_f ? 0 : 1;
  }
  if (S_ISDIR(st.st_mode)) {
    if (!opt_r) {
      fputs("rm: ", stderr);
      fputs(path, stderr);
      fputs(": is a directory\n", stderr);
      return 1;
    }
    int rc = remove_dir_contents(path);
    if (rmdir(path) < 0) {
      err_path(path);
      return 1;
    }
    return rc;
  }
  if (unlink(path) < 0) {
    err_path(path);
    return 1;
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
    if (strcmp(argv[argi], "--help") == 0) {
      fputs(
          "Usage: rm [-rRf] FILE...\n"
          "  -r, -R  Recursively remove directories\n"
          "  -f      Ignore nonexistent files\n", stdout);
      return 0;
    }
    const char *p = argv[argi] + 1;
    while (*p) {
      switch (*p) {
        case 'r':
        case 'R':
          opt_r = 1;
          break;
        case 'f':
          opt_f = 1;
          break;
        default:
          fputs("rm: unknown option: -", stderr);
          putchar(*p);
          fputs("\n", stderr);
          return 1;
      }
      p++;
    }
    argi++;
  }

  if (argi >= argc) {
    if (opt_f) return 0;
    fputs("rm: missing operand\n", stderr);
    return 1;
  }

  int rc = 0;
  for (int i = argi; i < argc; i++) {
    if (remove_any(argv[i])) rc = 1;
  }
  return rc;
}
