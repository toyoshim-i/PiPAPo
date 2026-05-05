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
  for (int i = len; i < width; i++) putchar(' ');
  fputs(&tmp[pos], stdout);
}

/* Print a human-readable size (KB input). */
static void print_human(uint32_t kb, int width) {
  char buf[12];
  if (kb >= 1024) {
    uint32_t mb = kb / 1024;
    uint32_t frac = (kb % 1024) * 10 / 1024; /* one decimal */
    snprintf(buf, (int)sizeof(buf), "%u.%uM", mb, frac);
  } else {
    snprintf(buf, (int)sizeof(buf), "%uK", kb);
  }
  int len = strlen(buf);
  for (int i = len; i < width; i++) putchar(' ');
  fputs(buf, stdout);
}

static void print_val(uint32_t kb, int width) {
  if (opt_human)
    print_human(kb, width);
  else
    print_right(kb, width);
}

static void print_row(const char *fs, uint32_t total, uint32_t used,
                      uint32_t free_kb, const char *mount) {
  fputs(C_BCYAN, stdout);
  int flen = strlen(fs);
  fputs(fs, stdout);
  fputs(C_RST, stdout);
  for (int i = flen; i < 12; i++) putchar(' ');

  fputs(C_WHITE, stdout);
  print_val(total, 8);
  fputs(C_RST, stdout);
  fputs(C_YELLOW, stdout);
  print_val(used, 8);
  fputs(C_RST, stdout);
  fputs(C_GREEN, stdout);
  print_val(free_kb, 8);
  fputs(C_RST, stdout);

  /* Use% — gradient: green <50%, yellow 50-89%, red >=90% */
  if (total > 0) {
    uint32_t pct = (used * 100 + total / 2) / total;
    if (pct >= 90) fputs(C_BRED, stdout);
    else if (pct >= 50) fputs(C_BYELLOW, stdout);
    else fputs(C_BGREEN, stdout);
    print_right(pct, 5);
    putchar('%');
    fputs(C_RST, stdout);
  } else {
    fputs("    - ", stdout);
  }

  fputs("  ", stdout);
  fputs(C_BWHITE, stdout);
  fputs(mount, stdout);
  fputs(C_RST, stdout);
  putchar('\n');
}

int main(int argc, char *argv[]) {
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-') {
    if (strcmp(argv[argi], "--help") == 0) {
      fputs(
          "Usage: df [-h] [--no-color]\n"
          "  -h  Human-readable sizes (K, M)\n"
          "  --no-color  Disable color output\n"
          "Shows filesystem usage from /proc/mounts.\n", stdout);
      return 0;
    }
    if (strcmp(argv[argi], "--no-color") == 0) {
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
          fputs("df: unknown option: -", stderr);
          putchar(*p);
          fputs("\n", stderr);
          return 1;
      }
      p++;
    }
    argi++;
  }

  char buf[512];

  fputs(C_REV, stdout);
  fputs(C_BOLD, stdout);
  if (opt_human)
    fputs("Filesystem    Total    Used    Free Use%  Mounted on", stdout);
  else
    fputs("Filesystem   Total(K) Used(K)  Free(K) Use%  Mounted on", stdout);
  fputs(C_RST, stdout);
  putchar('\n');

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
