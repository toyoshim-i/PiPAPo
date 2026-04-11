/*
 * top.c — display running processes with CPU/memory usage
 *
 * Usage: top [-c|-m|-p|-t] [-n COUNT]
 *   -c  sort by CPU usage (default)
 *   -m  sort by memory
 *   -p  sort by PID
 *   -t  sort by time
 *   -n  refresh COUNT times then exit (0 = infinite)
 */

#include "lib/uclib.h"
#include "common/termios.h"

/* Maximum processes we track (matches kernel PROC_MAX) */
#define MAX_PROCS 16

enum sort_mode { SORT_CPU, SORT_MEM, SORT_PID, SORT_TIME };

struct proc_info {
  int pid;
  int ppid;
  char state;
  char comm[32];
  uint32_t utime;
  uint32_t stime;
  uint32_t vsz;
  /* derived */
  uint32_t cpu_delta; /* ticks since last sample */
};

/* Previous per-PID tick snapshots for CPU% delta */
static int prev_pid[MAX_PROCS];
static uint32_t prev_ticks[MAX_PROCS];
static int prev_count;

static int parse_stat(const char *buf, struct proc_info *p) {
  const char *s = buf;
  p->pid = uc_atoi(s);
  while (*s && *s != '(') s++;
  if (!*s) return -1;
  s++;
  int i = 0;
  while (*s && *s != ')') {
    if (i < (int)sizeof(p->comm) - 1) p->comm[i++] = *s;
    s++;
  }
  p->comm[i] = '\0';
  if (!*s) return -1;
  s++;
  while (*s == ' ') s++;
  p->state = *s++;
  while (*s == ' ') s++;
  p->ppid = uc_atoi(s);

  /* skip to field 14 (utime): currently at field 4 digits, skip 10 spaces */
  int skip = 10;
  while (skip > 0 && *s) {
    if (*s == ' ') skip--;
    s++;
  }
  p->utime = 0;
  while (*s >= '0' && *s <= '9') {
    p->utime = p->utime * 10 + (uint32_t)(*s - '0');
    s++;
  }
  while (*s == ' ') s++;

  /* field 15: stime */
  p->stime = 0;
  while (*s >= '0' && *s <= '9') {
    p->stime = p->stime * 10 + (uint32_t)(*s - '0');
    s++;
  }

  /* skip to field 23 (vsize): skip 8 spaces (fields 16-23) */
  skip = 8;
  while (skip > 0 && *s) {
    if (*s == ' ') skip--;
    s++;
  }
  p->vsz = 0;
  while (*s >= '0' && *s <= '9') {
    p->vsz = p->vsz * 10 + (uint32_t)(*s - '0');
    s++;
  }
  return 0;
}

static uint32_t prev_lookup(int pid) {
  for (int i = 0; i < prev_count; i++)
    if (prev_pid[i] == pid) return prev_ticks[i];
  return 0;
}

static void prev_save(struct proc_info *procs, int n) {
  prev_count = n < MAX_PROCS ? n : MAX_PROCS;
  for (int i = 0; i < prev_count; i++) {
    prev_pid[i] = procs[i].pid;
    prev_ticks[i] = procs[i].utime + procs[i].stime;
  }
}

/* Parse "MemTotal:  NNN kB\nMemFree:  NNN kB\n..." */
static void parse_meminfo(const char *buf, uint32_t *total, uint32_t *free_kb) {
  *total = 0;
  *free_kb = 0;
  const char *p = buf;
  /* MemTotal */
  while (*p && *p != ':') p++;
  if (*p) p++;
  while (*p == ' ') p++;
  while (*p >= '0' && *p <= '9') {
    *total = *total * 10 + (uint32_t)(*p - '0');
    p++;
  }
  /* MemFree */
  while (*p && *p != ':') p++;
  if (*p) p++;
  while (*p == ' ') p++;
  while (*p >= '0' && *p <= '9') {
    *free_kb = *free_kb * 10 + (uint32_t)(*p - '0');
    p++;
  }
}

/* Parse "cpu USER 0 SYS IDLE ..." */
static void parse_cpu_stat(const char *buf, uint32_t *user, uint32_t *sys,
                           uint32_t *idle) {
  const char *p = buf;
  /* skip "cpu " */
  while (*p && *p != ' ') p++;
  while (*p == ' ') p++;
  *user = 0;
  while (*p >= '0' && *p <= '9') {
    *user = *user * 10 + (uint32_t)(*p - '0');
    p++;
  }
  while (*p == ' ') p++;
  /* skip nice */
  while (*p >= '0' && *p <= '9') p++;
  while (*p == ' ') p++;
  *sys = 0;
  while (*p >= '0' && *p <= '9') {
    *sys = *sys * 10 + (uint32_t)(*p - '0');
    p++;
  }
  while (*p == ' ') p++;
  *idle = 0;
  while (*p >= '0' && *p <= '9') {
    *idle = *idle * 10 + (uint32_t)(*p - '0');
    p++;
  }
}

static int read_file(const char *path, char *buf, int bufsz) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) return -1;
  ssize_t n = read(fd, buf, (size_t)(bufsz - 1));
  close(fd);
  if (n <= 0) return -1;
  buf[n] = '\0';
  return (int)n;
}

/* Sort index array to avoid struct copies (which emit memcpy) */
static int sort_idx[MAX_PROCS];

static void sort_procs(struct proc_info *p, int n, enum sort_mode mode) {
  for (int i = 0; i < n; i++) sort_idx[i] = i;
  /* insertion sort on indices */
  for (int i = 1; i < n; i++) {
    int key = sort_idx[i];
    int j = i - 1;
    while (j >= 0) {
      int cmp;
      int sj = sort_idx[j];
      switch (mode) {
        case SORT_CPU:
          cmp = p[sj].cpu_delta < p[key].cpu_delta;
          break;
        case SORT_MEM:
          cmp = p[sj].vsz < p[key].vsz;
          break;
        case SORT_TIME:
          cmp = (p[sj].utime + p[sj].stime) < (p[key].utime + p[key].stime);
          break;
        case SORT_PID:
        default:
          cmp = p[sj].pid > p[key].pid;
          break;
      }
      if (!cmp) break;
      sort_idx[j + 1] = sort_idx[j];
      j--;
    }
    sort_idx[j + 1] = key;
  }
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

static void print_time(uint32_t ticks) {
  uint32_t secs = ticks / 100;
  uint32_t mins = secs / 60;
  secs %= 60;
  char buf[8];
  uc_snprintf(buf, (int)sizeof(buf), "%2u:%02u", mins, secs);
  uc_puts(buf);
}

static const char *sort_name(enum sort_mode m) {
  switch (m) {
    case SORT_CPU: return "CPU";
    case SORT_MEM: return "MEM";
    case SORT_PID: return "PID";
    case SORT_TIME: return "TIME";
  }
  return "?";
}

int main(int argc, char *argv[]) {
  enum sort_mode mode = SORT_CPU;
  int iterations = 0; /* 0 = infinite */

  for (int i = 1; i < argc; i++) {
    if (uc_strcmp(argv[i], "--help") == 0) {
usage:
      uc_puts(
          "Usage: top [-c|-m|-p|-t] [-n COUNT]\n"
          "Options:\n"
          "  -c  sort by CPU usage (default)\n"
          "  -m  sort by memory\n"
          "  -p  sort by PID\n"
          "  -t  sort by time\n"
          "  -n  refresh COUNT times then exit (0 = infinite)\n"
          "Interactive keys:\n"
          "  c/P sort by CPU usage\n"
          "  m/M sort by memory\n"
          "  p/N sort by PID\n"
          "  t/T sort by time\n"
          "  o/O cycle sort field\n"
          "  q   quit\n");
      return 0;
    } else if (uc_strcmp(argv[i], "-c") == 0) {
      mode = SORT_CPU;
    } else if (uc_strcmp(argv[i], "-m") == 0) {
      mode = SORT_MEM;
    } else if (uc_strcmp(argv[i], "-p") == 0) {
      mode = SORT_PID;
    } else if (uc_strcmp(argv[i], "-t") == 0) {
      mode = SORT_TIME;
    } else if (uc_strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
      i++;
      uc_parse_u32(argv[i], (uint32_t *)&iterations);
    } else {
      uc_puts("top: unknown option: ");
      uc_puts(argv[i]);
      uc_putc('\n');
      goto usage;
    }
  }

  /* Switch stdin to raw mode so 'q' works without Enter */
  struct termios orig_tios, raw_tios;
  ioctl(0, TCGETS, &orig_tios);
  raw_tios = orig_tios;
  raw_tios.c_lflag &= ~(ICANON | ECHO);
  ioctl(0, TCSETS, &raw_tios);

  /* CPU stat snapshots for delta */
  uint32_t prev_user = 0, prev_sys = 0, prev_idle = 0;
  int first = 1;
  int count = 0;
  char buf[512];

  for (;;) {
    if (first)
      uc_puts("\033[2J"); /* clear screen on first display */
    uc_puts("\033[H");    /* cursor to 0,0 */

    /* Read uptime */
    uint32_t uptime_s = 0;
    if (read_file("/proc/uptime", buf, (int)sizeof(buf)) > 0)
      uptime_s = (uint32_t)uc_atoi(buf);

    /* Read meminfo */
    uint32_t mem_total = 0, mem_free = 0;
    if (read_file("/proc/meminfo", buf, (int)sizeof(buf)) > 0)
      parse_meminfo(buf, &mem_total, &mem_free);

    /* Read CPU stat */
    uint32_t cpu_user = 0, cpu_sys = 0, cpu_idle = 0;
    if (read_file("/proc/stat", buf, (int)sizeof(buf)) > 0)
      parse_cpu_stat(buf, &cpu_user, &cpu_sys, &cpu_idle);

    /* Compute CPU% deltas */
    uint32_t du = cpu_user - prev_user;
    uint32_t ds = cpu_sys - prev_sys;
    uint32_t di = cpu_idle - prev_idle;
    uint32_t dt = du + ds + di;
    if (first) dt = 0; /* no delta on first pass */
    prev_user = cpu_user;
    prev_sys = cpu_sys;
    prev_idle = cpu_idle;

    /* Enumerate processes */
    struct proc_info procs[MAX_PROCS];
    int nprocs = 0;

    int dfd = open("/proc", O_RDONLY, 0);
    if (dfd >= 0) {
      struct dirent de;
      while (getdents(dfd, &de, sizeof(de)) > 0 && nprocs < MAX_PROCS) {
        if (de.d_name[0] < '1' || de.d_name[0] > '9') continue;
        char path[32];
        uc_snprintf(path, (int)sizeof(path), "/proc/%s/stat", de.d_name);
        if (read_file(path, buf, (int)sizeof(buf)) < 0) continue;
        if (parse_stat(buf, &procs[nprocs]) < 0) continue;

        /* CPU delta for this process */
        uint32_t cur = procs[nprocs].utime + procs[nprocs].stime;
        uint32_t old = prev_lookup(procs[nprocs].pid);
        procs[nprocs].cpu_delta = cur - old;
        nprocs++;
      }
      close(dfd);
    }

    /* Save snapshots for next round */
    prev_save(procs, nprocs);

    /* Sort */
    sort_procs(procs, nprocs, mode);

    /* Header */
    uc_puts("top - up ");
    {
      uint32_t h = uptime_s / 3600;
      uint32_t m = (uptime_s % 3600) / 60;
      uint32_t s = uptime_s % 60;
      char tbuf[16];
      uc_snprintf(tbuf, (int)sizeof(tbuf), "%u:%02u:%02u", h, m, s);
      uc_puts(tbuf);
    }
    uc_puts(", ");
    uc_putu((uint32_t)nprocs);
    uc_puts(" procs  [sort: ");
    uc_puts(sort_name(mode));
    uc_puts("]\033[K\n");

    /* Memory line */
    uc_puts("Mem: ");
    uc_putu(mem_total);
    uc_puts("K total, ");
    uc_putu(mem_free);
    uc_puts("K free\033[K\n");

    /* CPU line */
    if (dt > 0) {
      uint32_t us_pct10 = du * 1000 / dt;
      uint32_t sy_pct10 = ds * 1000 / dt;
      uint32_t id_pct10 = di * 1000 / dt;
      char cpubuf[48];
      uc_snprintf(cpubuf, (int)sizeof(cpubuf), "%%Cpu: %u.%u us, %u.%u sy, %u.%u id\n",
                  us_pct10 / 10, us_pct10 % 10,
                  sy_pct10 / 10, sy_pct10 % 10,
                  id_pct10 / 10, id_pct10 % 10);
      uc_puts(cpubuf);
    } else {
      uc_puts("%Cpu: --\n");
    }

    /* Process table header */
    uc_puts("\n  PID  PPID S  %CPU  TIME    MEM COMMAND\n");

    /* Process rows */
    for (int i = 0; i < nprocs; i++) {
      int si = sort_idx[i];
      print_right((uint32_t)procs[si].pid, 5);
      print_right((uint32_t)procs[si].ppid, 6);
      uc_putc(' ');
      uc_putc(procs[si].state);
      /* %CPU: cpu_delta / dt * 100, show as XX.X */
      if (dt > 0) {
        uint32_t pct10 = procs[si].cpu_delta * 1000 / dt;
        char pctbuf[8];
        uc_snprintf(pctbuf, (int)sizeof(pctbuf), "%3u.%u", pct10 / 10,
                    pct10 % 10);
        uc_puts(pctbuf);
      } else {
        uc_puts("  --");
      }
      uc_putc(' ');
      print_time(procs[si].utime + procs[si].stime);
      print_right(procs[si].vsz / 1024, 7);
      uc_puts("K ");
      uc_puts(procs[si].comm);
      uc_puts("\033[K\n");
    }

    /* Clear any stale lines below (ESC[J = erase from cursor to end) */
    uc_puts("\033[J");

    count++;
    if (iterations > 0 && count >= iterations) break;

    /* Wait 1 second, but quit immediately on 'q' */
    {
      struct pollfd pfd;
      pfd.fd = 0; /* stdin */
      pfd.events = POLLIN;
      pfd.revents = 0;
      struct { long tv_sec; long tv_nsec; } ts = {1, 0};
      int r = ppoll(&pfd, 1, &ts, (void *)0, 0);
      if (r > 0) {
        char ch;
        if (read(0, &ch, 1) == 1) {
          if (ch == 'q' || ch == 'Q') break;
          if (ch == 'c' || ch == 'C' || ch == 'P') mode = SORT_CPU;
          if (ch == 'm' || ch == 'M') mode = SORT_MEM;
          if (ch == 'p' || ch == 'N') mode = SORT_PID;
          if (ch == 't' || ch == 'T') mode = SORT_TIME;
          if (ch == 'o' || ch == 'O') {
            if (mode == SORT_CPU) mode = SORT_MEM;
            else if (mode == SORT_MEM) mode = SORT_TIME;
            else if (mode == SORT_TIME) mode = SORT_PID;
            else mode = SORT_CPU;
          }
        }
      }
    }
    first = 0;
  }

  /* Restore terminal mode */
  ioctl(0, TCSETS, &orig_tios);
  return 0;
}
