/*
 * umount.c — unmount a filesystem
 *
 * Usage: umount TARGET
 *
 * Calls umount2(target, 0).  PPAP's VFS performs a synchronous
 * unmount; -f / -l flags are accepted by busybox but PPAP treats
 * every unmount the same way, so we don't expose them yet.
 */

#include "lib/uclib.h"

int main(int argc, char *argv[]) {
  if (argc != 2 || strcmp(argv[1], "--help") == 0) {
    fputs("Usage: umount TARGET\n", stderr);
    return (argc != 2) ? 1 : 0;
  }
  if (umount2(argv[1], 0) < 0) {
    fputs("umount: failed (target not mounted, busy, or invalid)\n", stderr);
    return 1;
  }
  return 0;
}
