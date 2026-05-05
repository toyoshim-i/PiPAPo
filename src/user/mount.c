/*
 * mount.c — mount filesystems / list mounts
 *
 * Usage:
 *   mount                          list active mounts (reads /proc/mounts)
 *   mount -t TYPE [SRC] TARGET     mount TYPE at TARGET (with optional SRC)
 *   mount -t TYPE [SRC] TARGET -r  mount read-only
 *
 * SRC may be omitted for pseudo-filesystems (procfs / devfs / tmpfs);
 * pass it for filesystems that need a backing device (vfat, ufs, romfs).
 *
 * Not implemented: -o option-list parsing, -a all-from-fstab, remount.
 * Use busybox if you need them.
 */

#include "lib/uclib.h"

static int use_color = 1;
#define C(seq) (use_color ? (seq) : "")
#define C_RST    C("\033[0m")
#define C_BCYAN  C("\033[1;36m")
#define C_BWHITE C("\033[1;37m")
#define C_DIM    C("\033[2m")

static int read_file(const char *path, char *buf, int bufsz) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, (size_t)(bufsz - 1));
  close(fd);
  if (n < 0) return -1;
  buf[n] = '\0';
  return (int)n;
}

/* /proc/mounts format per line: "device mountpoint fstype options 0 0\n".
 * Print as: "device on mountpoint type fstype (options)" — Linux idiom. */
static void list_mounts(void) {
  char buf[1024];
  int n = read_file("/proc/mounts", buf, (int)sizeof(buf));
  if (n < 0) {
    uc_eputs("mount: cannot read /proc/mounts\n");
    return;
  }
  const char *p = buf;
  while (*p) {
    /* Split into 4 fields by space. */
    const char *fields[4];
    int flen[4];
    int found = 0;
    for (int i = 0; i < 4 && *p && *p != '\n'; i++) {
      fields[i] = p;
      while (*p && *p != ' ' && *p != '\n') p++;
      flen[i] = (int)(p - fields[i]);
      if (*p == ' ') p++;
      found = i + 1;
    }
    /* Skip remainder ("0 0") and the newline. */
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;

    if (found < 4) continue;

    uc_puts(C_BCYAN);
    for (int i = 0; i < flen[0]; i++) putchar(fields[0][i]);
    uc_puts(C_RST);
    uc_puts(" on ");
    uc_puts(C_BWHITE);
    for (int i = 0; i < flen[1]; i++) putchar(fields[1][i]);
    uc_puts(C_RST);
    uc_puts(" type ");
    for (int i = 0; i < flen[2]; i++) putchar(fields[2][i]);
    uc_puts(" (");
    uc_puts(C_DIM);
    for (int i = 0; i < flen[3]; i++) putchar(fields[3][i]);
    uc_puts(C_RST);
    uc_puts(")\n");
  }
}

static void usage(void) {
  uc_eputs(
      "Usage: mount\n"
      "       mount -t TYPE [SRC] TARGET [-r]\n"
      "  -t TYPE     filesystem type (procfs / devfs / tmpfs / vfat / "
      "ufs / romfs)\n"
      "  -r          read-only mount\n"
      "  --no-color  disable color output\n");
}

int main(int argc, char *argv[]) {
  const char *fstype = 0;
  const char *src = 0;
  const char *tgt = 0;
  long flags = 0;

  int argi = 1;
  /* Two-pass: first pull -t/-r/--no-color, then collect positional args. */
  while (argi < argc) {
    const char *a = argv[argi];
    if (strcmp(a, "--help") == 0) {
      usage();
      return 0;
    }
    if (strcmp(a, "--no-color") == 0) {
      use_color = 0;
      argi++;
      continue;
    }
    if (strcmp(a, "-r") == 0) {
      flags |= MS_RDONLY;
      argi++;
      continue;
    }
    if (strcmp(a, "-t") == 0) {
      if (argi + 1 >= argc) {
        uc_eputs("mount: -t requires a filesystem type\n");
        return 1;
      }
      fstype = argv[argi + 1];
      argi += 2;
      continue;
    }
    /* Positional: first becomes src, second becomes tgt.  If only one
     * positional arg is given, it's the target (typical for pseudo-FS). */
    if (!src) {
      src = a;
    } else if (!tgt) {
      tgt = a;
    } else {
      uc_eputs("mount: unexpected argument: ");
      uc_eputs(a);
      uc_eputs("\n");
      return 1;
    }
    argi++;
  }

  /* No -t and no positional args -> list. */
  if (!fstype && !src && !tgt) {
    list_mounts();
    return 0;
  }

  /* Promote single positional to target. */
  if (src && !tgt) {
    tgt = src;
    src = 0;
  }

  if (!fstype || !tgt) {
    uc_eputs("mount: -t TYPE and TARGET are required\n");
    return 1;
  }

  if (mount(src, tgt, fstype, flags, 0) < 0) {
    uc_eputs("mount: failed (check fstype, target dir exists, source if "
             "required)\n");
    return 1;
  }
  return 0;
}
