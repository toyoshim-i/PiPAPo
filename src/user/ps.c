/*
 * ps.c — report process status
 *
 * Usage: ps
 * Lists all processes by reading /proc/<pid>/stat.
 */

#include "lib/uclib.h"

/* Parse pid, comm, state, ppid, and vsize from /proc/<pid>/stat.
 * Format: "<pid> (<comm>) <state> <ppid> ... <vsize(23)> ..." */
static int parse_stat(const char *buf, int *pid, char *comm, int commsz,
                      char *state, int *ppid, uint32_t *vsz) {
  const char *p = buf;

  /* Field 1: pid */
  *pid = uc_atoi(p);
  while (*p && *p != '(') p++;
  if (!*p) return -1;
  p++; /* skip '(' */

  /* Field 2: comm — text between '(' and ')' */
  int i = 0;
  while (*p && *p != ')') {
    if (i < commsz - 1) comm[i++] = *p;
    p++;
  }
  comm[i] = '\0';
  if (!*p) return -1;
  p++; /* skip ')' */
  while (*p == ' ') p++;

  /* Field 3: state */
  *state = *p++;
  while (*p == ' ') p++;

  /* Field 4: ppid */
  *ppid = uc_atoi(p);

  /* Skip to field 23 (vsize): need to skip fields 5-22 (18 more fields) */
  int skip = 18;
  while (skip > 0 && *p) {
    if (*p == ' ') skip--;
    p++;
  }
  /* Now at field 23 */
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

int main(void) {
  uc_puts("  PID  PPID S    MEM COMMAND\n");

  int dfd = open("/proc", O_RDONLY, 0);
  if (dfd < 0) return 1;

  struct dirent de;
  while (getdents(dfd, &de, sizeof(de)) > 0) {
    /* Only numeric entries (pids). */
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
    uint32_t vsz;
    if (parse_stat(buf, &pid, comm, (int)sizeof(comm), &state, &ppid, &vsz) < 0)
      continue;

    print_right((uint32_t)pid, 5);
    print_right((uint32_t)ppid, 6);
    uc_putc(' ');
    uc_putc(state);
    print_right(vsz / 1024, 7);
    uc_puts("K ");
    uc_puts(comm);
    uc_putc('\n');
  }

  close(dfd);
  return 0;
}
