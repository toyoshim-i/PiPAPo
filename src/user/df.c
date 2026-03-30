/*
 * df.c — report filesystem disk space usage
 *
 * Usage: df
 * Reads /proc/mounts for mounted filesystems and /proc/meminfo for
 * RAM usage.  Per-filesystem statvfs is not yet available, so sizes
 * are only reported for RAM (tmpfs).
 */

#include "lib/uclib.h"

static int read_file(const char *path, char *buf, int bufsz) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, (size_t)(bufsz - 1));
  close(fd);
  if (n < 0) return -1;
  buf[n] = '\0';
  return (int)n;
}

/* Parse "Key:  <value> kB\n" from /proc/meminfo. */
static uint32_t parse_meminfo_kb(const char *buf, const char *key) {
  int klen = uc_strlen(key);
  const char *p = buf;
  while (*p) {
    if (uc_strncmp(p, key, klen) == 0) {
      p += klen;
      while (*p == ' ' || *p == ':') p++;
      uint32_t v = 0;
      while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint32_t)(*p - '0');
        p++;
      }
      return v;
    }
    while (*p && *p != '\n') p++;
    if (*p) p++;
  }
  return 0;
}

static void print_right(uint32_t v, int width) {
  char tmp[12];
  int pos = (int)sizeof(tmp) - 1;
  tmp[pos] = '\0';
  if (v == 0) {
    tmp[--pos] = '0';
  } else {
    while (v && pos > 0) {
      tmp[--pos] = (char)('0' + v % 10);
      v /= 10;
    }
  }
  int len = (int)sizeof(tmp) - 1 - pos;
  for (int i = len; i < width; i++) uc_putc(' ');
  uc_puts(&tmp[pos]);
}

/* Print a row: Filesystem, Total, Used, Free, Mounted on. */
static void print_row(const char *fs, uint32_t total, uint32_t used,
                      uint32_t free_kb, const char *mount) {
  /* Pad filesystem name to 12 chars. */
  int flen = uc_strlen(fs);
  uc_puts(fs);
  for (int i = flen; i < 12; i++) uc_putc(' ');

  print_right(total, 8);
  print_right(used, 8);
  print_right(free_kb, 8);
  uc_puts("  ");
  uc_puts(mount);
  uc_putc('\n');
}

int main(void) {
  char buf[512];

  /* Read meminfo for RAM totals. */
  uint32_t mem_total = 0, mem_free = 0;
  if (read_file("/proc/meminfo", buf, (int)sizeof(buf)) > 0) {
    mem_total = parse_meminfo_kb(buf, "MemTotal");
    mem_free = parse_meminfo_kb(buf, "MemFree");
  }

  uc_puts("Filesystem   Total(K) Used(K)  Free(K)  Mounted on\n");

  /* Parse /proc/mounts: "<dev> <mnt> <fstype> <opts> 0 0\n" */
  if (read_file("/proc/mounts", buf, (int)sizeof(buf)) <= 0) return 1;

  const char *p = buf;
  while (*p) {
    /* Parse one line: fstype mountpoint fstype opts 0 0 */
    char fstype[16], mount[32];
    int i;

    /* Field 1: device/fstype name. */
    i = 0;
    while (*p && *p != ' ') {
      if (i < (int)sizeof(fstype) - 1) fstype[i++] = *p;
      p++;
    }
    fstype[i] = '\0';
    while (*p == ' ') p++;

    /* Field 2: mount point. */
    i = 0;
    while (*p && *p != ' ') {
      if (i < (int)sizeof(mount) - 1) mount[i++] = *p;
      p++;
    }
    mount[i] = '\0';

    /* Skip remaining fields to next line. */
    while (*p && *p != '\n') p++;
    if (*p) p++;

    /* Show sizes for tmpfs (backed by RAM). */
    if (uc_strcmp(fstype, "tmpfs") == 0) {
      print_row(fstype, mem_total, mem_total - mem_free, mem_free, mount);
    } else {
      /* No per-fs size info available yet. */
      print_row(fstype, 0, 0, 0, mount);
    }
  }

  return 0;
}
