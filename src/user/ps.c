/*
 * ps.c — report process status
 *
 * Usage: ps [-a]
 * -a: show all processes (default without -a: all, since we have no
 *     session filtering yet — kept for compatibility).
 * Columns: PID, PPID, S(tate), TIME (utime+stime in seconds), MEM, COMMAND.
 */

#include "lib/uclib.h"

/* Parse fields from /proc/<pid>/stat (Linux 52-field format).
 * Extracts: pid(1), comm(2), state(3), ppid(4), utime(14), stime(15),
 *           vsize(23). */
static int parse_stat(const char *buf, int *pid, char *comm, int commsz,
                      char *state, int *ppid, uint32_t *utime, uint32_t *stime,
                      uint32_t *vsz) {
  const char *p = buf;

  *pid = uc_atoi(p);
  while (*p && *p != '(') p++;
  if (!*p) return -1;
  p++;

  int i = 0;
  while (*p && *p != ')') {
    if (i < commsz - 1) comm[i++] = *p;
    p++;
  }
  comm[i] = '\0';
  if (!*p) return -1;
  p++;
  while (*p == ' ') p++;

  *state = *p++;
  while (*p == ' ') p++;

  *ppid = uc_atoi(p);

  /* Advance to field 14 (utime): skip fields 5-13 (9 fields). */
  int skip = 9;
  while (skip > 0 && *p) {
    if (*p == ' ') skip--;
    p++;
  }
  *utime = 0;
  while (*p >= '0' && *p <= '9') {
    *utime = *utime * 10 + (uint32_t)(*p - '0');
    p++;
  }
  while (*p == ' ') p++;

  /* Field 15: stime */
  *stime = 0;
  while (*p >= '0' && *p <= '9') {
    *stime = *stime * 10 + (uint32_t)(*p - '0');
    p++;
  }

  /* Advance to field 23 (vsize): skip fields 16-22 (7 fields). */
  skip = 7;
  while (skip > 0 && *p) {
    if (*p == ' ') skip--;
    p++;
  }
  *vsz = 0;
  while (*p >= '0' && *p <= '9') {
    *vsz = *vsz * 10 + (uint32_t)(*p - '0');
    p++;
  }
  return 0;
}

static void print_right(uint32_t v, int width) {
  char buf[12];
  int pos = (int)sizeof(buf) - 1;
  buf[pos] = '\0';
  if (v == 0) {
    buf[--pos] = '0';
  } else {
    while (v && pos > 0) {
      buf[--pos] = (char)('0' + v % 10);
      v /= 10;
    }
  }
  int len = (int)sizeof(buf) - 1 - pos;
  for (int i = len; i < width; i++) uc_putc(' ');
  uc_puts(&buf[pos]);
}

/* Format ticks as MM:SS (100 ticks/sec). */
static void print_time(uint32_t ticks) {
  uint32_t secs = ticks / 100;
  uint32_t mins = secs / 60;
  secs %= 60;
  char buf[8];
  uc_snprintf(buf, (int)sizeof(buf), "%2u:%02u", mins, secs);
  uc_puts(buf);
}

int main(int argc, char *argv[]) {
  if (argc > 1 && uc_strcmp(argv[1], "--help") == 0) {
    uc_puts(
        "Usage: ps\n"
        "List all processes.\n"
        "Columns: PID, PPID, State, TIME, MEM, COMMAND\n");
    return 0;
  }

  uc_puts("  PID  PPID S  TIME    MEM COMMAND\n");

  int dfd = open("/proc", O_RDONLY, 0);
  if (dfd < 0) return 1;

  struct dirent de;
  while (getdents(dfd, &de, sizeof(de)) > 0) {
    if (de.d_name[0] < '1' || de.d_name[0] > '9') continue;

    char path[32];
    uc_snprintf(path, (int)sizeof(path), "/proc/%s/stat", de.d_name);

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) continue;

    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) continue;
    buf[n] = '\0';

    int pid, ppid;
    char comm[32], state;
    uint32_t utime, stime, vsz;
    if (parse_stat(buf, &pid, comm, (int)sizeof(comm), &state, &ppid, &utime,
                   &stime, &vsz) < 0)
      continue;

    print_right((uint32_t)pid, 5);
    print_right((uint32_t)ppid, 6);
    uc_putc(' ');
    uc_putc(state);
    uc_putc(' ');
    print_time(utime + stime);
    print_right(vsz / 1024, 7);
    uc_puts("K ");
    uc_puts(comm);
    uc_putc('\n');
  }

  close(dfd);
  return 0;
}
