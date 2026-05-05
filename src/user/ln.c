/*
 * ln.c — create a hard link
 *
 * Usage: ln TARGET [LINK_NAME]
 *
 * Hard link only.  Symbolic links (ln -s) require VFS symlink write
 * support that isn't there yet; the busybox ln covers that case.
 *
 * If LINK_NAME is omitted, creates a link in the current directory
 * using TARGET's basename — standard POSIX convenience.
 *
 * The underlying VFS path_link op only works on filesystems that
 * can model hard links (UFS today).  On tmpfs, devfs, procfs, vfat,
 * and romfs it returns -EPERM and we report the error.
 */

#include "lib/uclib.h"

static int use_color = 1;
#define C(seq) (use_color ? (seq) : "")
#define C_RST C("\033[0m")
#define C_RED C("\033[31m")

int main(int argc, char *argv[]) {
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
    if (strcmp(argv[argi], "--help") == 0) {
      fputs(
          "Usage: ln [--no-color] TARGET [LINK_NAME]\n"
          "  Create a hard link LINK_NAME pointing at TARGET.\n"
          "  If LINK_NAME is omitted, use TARGET's basename in the cwd.\n"
          "  Symbolic links (-s) are not supported — use busybox ln.\n", stdout);
      return 0;
    }
    if (strcmp(argv[argi], "--no-color") == 0) {
      use_color = 0;
      argi++;
      continue;
    }
    if (strcmp(argv[argi], "-s") == 0) {
      fputs("ln: -s (symbolic link) not supported; use busybox ln\n", stderr);
      return 1;
    }
    fputs("ln: unknown option: ", stderr);
    fputs(argv[argi], stderr);
    fputs("\n", stderr);
    return 1;
  }

  if (argi >= argc) {
    fputs("ln: missing TARGET\n", stderr);
    return 1;
  }

  const char *target = argv[argi++];
  const char *linkname;
  if (argi < argc) {
    linkname = argv[argi++];
  } else {
    /* Default to target's basename in cwd. */
    linkname = uc_basename(target);
  }

  if (argi != argc) {
    fputs("ln: too many operands\n", stderr);
    return 1;
  }

  if (link(target, linkname) < 0) {
    fputs(C_RED, stderr);
    fputs("ln: cannot create link: ", stderr);
    fputs(C_RST, stderr);
    fputs(linkname, stderr);
    fputs(" -> ", stderr);
    fputs(target, stderr);
    fputs("\n", stderr);
    return 1;
  }
  return 0;
}
