/*
 * free.c — display kernel memory usage
 *
 * Usage: free [-h] [--no-color]
 *   -h            human-readable sizes (K / M)
 *   --no-color    plain output
 *
 * Reads /proc/meminfo (PPAP format: MemTotal / MemFree / PageSize /
 * DataMax / OomCount).  The PPAP kernel has no buffer/cache layer
 * and no swap, so the table is just total / used / free for the
 * RAM_STACK region, plus the page-allocator metadata as a second
 * line.
 */

#include "lib/uclib.h"

static int opt_human;
static int use_color = 1;
#define C(seq) (use_color ? (seq) : "")
#define C_RST     C("\033[0m")
#define C_GREEN   C("\033[32m")
#define C_YELLOW  C("\033[33m")
#define C_WHITE   C("\033[37m")
#define C_BRED    C("\033[1;31m")
#define C_BGREEN  C("\033[1;32m")
#define C_BYELLOW C("\033[1;33m")
#define C_BCYAN   C("\033[1;36m")
#define C_BWHITE  C("\033[1;37m")
#define C_REV     C("\033[7m")
#define C_BOLD    C("\033[1m")

static int read_file(const char *path, char *buf, int bufsz) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, (size_t)(bufsz - 1));
  close(fd);
  if (n < 0) return -1;
  buf[n] = '\0';
  return (int)n;
}

/* Find the first line beginning with `key:` and parse the next
 * decimal number.  Returns -1 if not found. */
static int32_t lookup_u32(const char *buf, const char *key) {
  int klen = strlen(key);
  const char *p = buf;
  while (*p) {
    if (strncmp(p, key, klen) == 0 && p[klen] == ':') {
      p += klen + 1;
      while (*p == ' ' || *p == '\t') p++;
      uint32_t v = 0;
      if (*p < '0' || *p > '9') return -1;
      while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p - '0');
        p++;
      }
      return (int32_t)v;
    }
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
  }
  return -1;
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

/* Human-readable size given a value in KB. */
static void print_human(uint32_t kb, int width) {
  char buf[12];
  if (kb >= 1024) {
    uint32_t mb = kb / 1024;
    uint32_t frac = (kb % 1024) * 10 / 1024;
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

int main(int argc, char *argv[]) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      fputs(
          "Usage: free [-h] [--no-color]\n"
          "  -h          Human-readable sizes (K, M)\n"
          "  --no-color  Disable color output\n"
          "Shows kernel memory usage from /proc/meminfo.\n", stdout);
      return 0;
    }
    if (strcmp(argv[i], "--no-color") == 0) {
      use_color = 0;
      continue;
    }
    if (strcmp(argv[i], "-h") == 0) {
      opt_human = 1;
      continue;
    }
    fputs("free: unknown argument: ", stderr);
    fputs(argv[i], stderr);
    fputs("\n", stderr);
    return 1;
  }

  char buf[512];
  if (read_file("/proc/meminfo", buf, (int)sizeof(buf)) < 0) {
    fputs("free: cannot read /proc/meminfo\n", stderr);
    return 1;
  }

  int32_t total_kb  = lookup_u32(buf, "MemTotal");
  int32_t free_kb   = lookup_u32(buf, "MemFree");
  int32_t pagesize  = lookup_u32(buf, "PageSize");
  int32_t datamax   = lookup_u32(buf, "DataMax");
  int32_t oomcount  = lookup_u32(buf, "OomCount");

  if (total_kb < 0 || free_kb < 0) {
    fputs("free: /proc/meminfo missing required fields\n", stderr);
    return 1;
  }
  uint32_t used_kb = (uint32_t)total_kb - (uint32_t)free_kb;

  /* Header */
  fputs(C_REV, stdout);
  fputs(C_BOLD, stdout);
  fputs("            total      used      free", stdout);
  fputs(C_RST, stdout);
  putchar('\n');

  /* Mem row */
  fputs(C_BCYAN, stdout);
  fputs("Mem:    ", stdout);
  fputs(C_RST, stdout);
  fputs(C_WHITE, stdout);
  print_val((uint32_t)total_kb, 9);
  fputs(C_RST, stdout);
  fputs(C_YELLOW, stdout);
  print_val(used_kb, 10);
  fputs(C_RST, stdout);
  /* Free colored by headroom: red if <10% free, yellow <25%, green otherwise. */
  uint32_t free_pct = (total_kb > 0)
      ? (uint32_t)((free_kb * 100u) / (uint32_t)total_kb)
      : 0;
  if (free_pct < 10) fputs(C_BRED, stdout);
  else if (free_pct < 25) fputs(C_BYELLOW, stdout);
  else fputs(C_BGREEN, stdout);
  print_val((uint32_t)free_kb, 10);
  fputs(C_RST, stdout);
  putchar('\n');

  /* Auxiliary line — page allocator state. */
  if (pagesize > 0 || datamax > 0 || oomcount >= 0) {
    fputs(C_BWHITE, stdout);
    fputs("Page: ", stdout);
    fputs(C_RST, stdout);
    if (pagesize > 0) {
      fputs(" size=", stdout);
      print_right((uint32_t)pagesize, 1);
      putchar('B');
    }
    if (datamax > 0) {
      fputs(" data_max=", stdout);
      print_right((uint32_t)datamax, 1);
      fputs("KB", stdout);
    }
    if (oomcount >= 0) {
      fputs(" oom=", stdout);
      if (oomcount > 0) fputs(C_BRED, stdout);
      print_right((uint32_t)oomcount, 1);
      if (oomcount > 0) fputs(C_RST, stdout);
    }
    putchar('\n');
  }

  return 0;
}
