/*
 * df.c — report filesystem disk space usage
 *
 * Usage: df [-h]
 * -h: human-readable sizes (K, M).
 * Reads /proc/mounts and /proc/meminfo.
 */

#include "lib/uclib.h"

static int opt_human;

static int read_file(const char *path, char *buf, int bufsz) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, (size_t)(bufsz - 1));
  close(fd);
  if (n < 0) return -1;
  buf[n] = '\0';
  return (int)n;
}

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

/* Print a human-readable size (KB input). */
static void print_human(uint32_t kb, int width) {
  char buf[12];
  if (kb >= 1024) {
    uint32_t mb = kb / 1024;
    uint32_t frac = (kb % 1024) * 10 / 1024; /* one decimal */
    uc_snprintf(buf, (int)sizeof(buf), "%u.%uM", mb, frac);
  } else {
    uc_snprintf(buf, (int)sizeof(buf), "%uK", kb);
  }
  int len = uc_strlen(buf);
  for (int i = len; i < width; i++) uc_putc(' ');
  uc_puts(buf);
}

static void print_val(uint32_t kb, int width) {
  if (opt_human)
    print_human(kb, width);
  else
    print_right(kb, width);
}

static void print_row(const char *fs, uint32_t total, uint32_t used,
                      uint32_t free_kb, const char *mount) {
  int flen = uc_strlen(fs);
  uc_puts(fs);
  for (int i = flen; i < 12; i++) uc_putc(' ');

  print_val(total, 8);
  print_val(used, 8);
  print_val(free_kb, 8);

  /* Use% */
  if (total > 0) {
    uint32_t pct = (used * 100 + total / 2) / total;
    print_right(pct, 5);
    uc_putc('%');
  } else {
    uc_puts("    - ");
  }

  uc_puts("  ");
  uc_puts(mount);
  uc_putc('\n');
}

int main(int argc, char *argv[]) {
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-') {
    if (uc_strcmp(argv[argi], "--help") == 0) {
      uc_puts(
          "Usage: df [-h]\n"
          "  -h  Human-readable sizes (K, M)\n"
          "Shows filesystem usage from /proc/mounts.\n");
      return 0;
    }
    const char *p = argv[argi] + 1;
    while (*p) {
      switch (*p) {
        case 'h':
          opt_human = 1;
          break;
        default:
          uc_eputs("df: unknown option: -");
          uc_putc(*p);
          uc_eputs("\n");
          return 1;
      }
      p++;
    }
    argi++;
  }

  char buf[512];

  uint32_t mem_total = 0, mem_free = 0;
  if (read_file("/proc/meminfo", buf, (int)sizeof(buf)) > 0) {
    mem_total = parse_meminfo_kb(buf, "MemTotal");
    mem_free = parse_meminfo_kb(buf, "MemFree");
  }

  if (opt_human)
    uc_puts("Filesystem    Total    Used    Free Use%  Mounted on\n");
  else
    uc_puts("Filesystem   Total(K) Used(K)  Free(K) Use%  Mounted on\n");

  if (read_file("/proc/mounts", buf, (int)sizeof(buf)) <= 0) return 1;

  const char *p = buf;
  while (*p) {
    char fstype[16], mount[32];
    int i;

    i = 0;
    while (*p && *p != ' ') {
      if (i < (int)sizeof(fstype) - 1) fstype[i++] = *p;
      p++;
    }
    fstype[i] = '\0';
    while (*p == ' ') p++;

    i = 0;
    while (*p && *p != ' ') {
      if (i < (int)sizeof(mount) - 1) mount[i++] = *p;
      p++;
    }
    mount[i] = '\0';

    while (*p && *p != '\n') p++;
    if (*p) p++;

    if (uc_strcmp(fstype, "tmpfs") == 0) {
      print_row(fstype, mem_total, mem_total - mem_free, mem_free, mount);
    } else {
      print_row(fstype, 0, 0, 0, mount);
    }
  }

  return 0;
}
