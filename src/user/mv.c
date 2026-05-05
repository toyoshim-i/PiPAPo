/*
 * mv.c — move / rename a file
 *
 * Usage: mv SOURCE DEST
 *
 * Tries rename(2) first — the fast path when source and destination
 * live on the same mount.  Across mounts (e.g. /mnt/ufs → /tmp)
 * PPAP's vfs_path_rename returns -ENOSYS because each filesystem's
 * .rename op only knows about its own inodes.  Same story for the
 * common "source is on a read-only rootfs" case → -EROFS.  In either
 * event, fall back to copy + unlink.
 *
 * No -i / -f flags yet; DEST is overwritten unconditionally.  No
 * directory moves (mv dir/ newdir/) because cp doesn't support -r
 * yet either.  Reach for busybox mv when those are needed.
 */

#include "lib/uclib.h"

static int use_color = 1;
#define C(seq) (use_color ? (seq) : "")
#define C_RST C("\033[0m")
#define C_RED C("\033[31m")

static void die(const char *prefix, const char *path) {
  fputs(C_RED, stderr);
  fputs("mv: ", stderr);
  fputs(C_RST, stderr);
  fputs(prefix, stderr);
  fputs(path, stderr);
  fputs("\n", stderr);
  _exit(1);
}

static int copy_then_unlink(const char *src_path, const char *dst_path) {
  int src_fd = open(src_path, O_RDONLY, 0);
  if (src_fd < 0) return -1;

  int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (dst_fd < 0) {
    close(src_fd);
    return -1;
  }

  long n = uc_copy_fd(src_fd, dst_fd);
  close(src_fd);
  close(dst_fd);
  if (n < 0) {
    /* Partial / failed copy — try to clean up the half-written dst so
     * we don't leave a corrupt file behind.  If the unlink also fails,
     * there's nothing useful to do about it. */
    unlink(dst_path);
    return -1;
  }

  if (unlink(src_path) < 0) {
    /* Copy succeeded but source can't be removed (e.g. RDONLY mount).
     * Destination is now valid; leave it in place and report the
     * partial success — the caller decides if that's fatal. */
    return 1;
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
    if (strcmp(argv[argi], "--help") == 0) {
      fputs(
          "Usage: mv [--no-color] SOURCE DEST\n"
          "  Moves SOURCE to DEST (overwriting DEST if it exists).\n", stdout);
      return 0;
    }
    if (strcmp(argv[argi], "--no-color") == 0) {
      use_color = 0;
      argi++;
      continue;
    }
    fputs("mv: unknown option: ", stderr);
    fputs(argv[argi], stderr);
    fputs("\n", stderr);
    return 1;
  }

  if (argi + 2 != argc) {
    fputs("mv: expected SOURCE and DEST\n", stderr);
    return 1;
  }

  const char *src_path = argv[argi];
  const char *dst_path = argv[argi + 1];

  if (rename(src_path, dst_path) == 0) return 0;

  /* Fast path failed; try copy + unlink.  If the source doesn't exist
   * at all, stat will fail cleanly and we can give a better error
   * than copy_then_unlink's opaque "couldn't open". */
  struct stat st;
  if (stat(src_path, &st) < 0) die("source not found: ", src_path);

  int rc = copy_then_unlink(src_path, dst_path);
  if (rc < 0) die("copy failed: ", src_path);
  if (rc > 0) die("copied but could not remove source: ", src_path);
  return 0;
}
