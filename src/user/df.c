/*
 * df.c — report filesystem disk space usage
 *
 * Usage: df [-h]
 * -h: human-readable sizes (K, M).
 * Reads /proc/mounts and calls statfs64() per mount.
 */

#include "lib/uclib.h"

static int opt_human;
static int use_color = 1;
#define C(seq) (use_color ? (seq) : "")
#define C_RST     C("\033[0m")
#define C_BOLD    C("\033[1m")
#define C_GREEN   C("\033[32m")
#define C_YELLOW  C("\033[33m")
#define C_CYAN    C("\033[36m")
#define C_WHITE   C("\033[37m")
#define C_BRED    C("\033[1;31m")
#define C_BGREEN  C("\033[1;32m")
#define C_BYELLOW C("\033[1;33m")
#define C_BCYAN   C("\033[1;36m")
#define C_BWHITE  C("\033[1;37m")
#define C_REV     C("\033[7m")

static int read_file(const char *path, char *buf, int bufsz) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, (size_t)(bufsz - 1));
  close(fd);
  if (n < 0) return -1;
  buf[n] = '\0';
  return (int)n;
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
  uc_puts(C_BCYAN);
  int flen = uc_strlen(fs);
  uc_puts(fs);
  uc_puts(C_RST);
  for (int i = flen; i < 12; i++) uc_putc(' ');

  uc_puts(C_WHITE);
  print_val(total, 8);
  uc_puts(C_RST);
  uc_puts(C_YELLOW);
  print_val(used, 8);
  uc_puts(C_RST);
  uc_puts(C_GREEN);
  print_val(free_kb, 8);
  uc_puts(C_RST);

  /* Use% — gradient: green <50%, yellow 50-89%, red >=90% */
  if (total > 0) {
    uint32_t pct = (used * 100 + total / 2) / total;
    if (pct >= 90) uc_puts(C_BRED);
    else if (pct >= 50) uc_puts(C_BYELLOW);
    else uc_puts(C_BGREEN);
    print_right(pct, 5);
    uc_putc('%');
    uc_puts(C_RST);
  } else {
    uc_puts("    - ");
  }

  uc_puts("  ");
  uc_puts(C_BWHITE);
  uc_puts(mount);
  uc_puts(C_RST);
  uc_putc('\n');
}

int main(int argc, char *argv[]) {
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-') {
    if (uc_strcmp(argv[argi], "--help") == 0) {
      uc_puts(
          "Usage: df [-h] [--no-color]\n"
          "  -h  Human-readable sizes (K, M)\n"
          "  --no-color  Disable color output\n"
          "Shows filesystem usage from /proc/mounts.\n");
      return 0;
    }
    if (uc_strcmp(argv[argi], "--no-color") == 0) {
      use_color = 0;
      argi++;
      continue;
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

  uc_puts(C_REV);
  uc_puts(C_BOLD);
  if (opt_human)
    uc_puts("Filesystem    Total    Used    Free Use%  Mounted on");
  else
    uc_puts("Filesystem   Total(K) Used(K)  Free(K) Use%  Mounted on");
  uc_puts(C_RST);
  uc_putc('\n');

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

    struct statfs sf;
    uint32_t total_kb = 0, free_kb = 0, used_kb = 0;
    if (statfs64(mount, (long)sizeof(sf), &sf) == 0 && sf.f_bsize > 0) {
      uint32_t bsize = sf.f_bsize;
      total_kb = (uint32_t)(sf.f_blocks * bsize / 1024);
      free_kb = (uint32_t)(sf.f_bavail * bsize / 1024);
      used_kb = total_kb - free_kb;
    }
    print_row(fstype, total_kb, used_kb, free_kb, mount);
  }

  return 0;
}
