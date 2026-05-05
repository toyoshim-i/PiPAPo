/*
 * date.c — print or set the system wallclock
 *
 * Usage:
 *   date              Print current time in UTC: "YYYY-MM-DD HH:MM:SS UTC"
 *   date -s EPOCH     Set system time to EPOCH (seconds since Unix epoch)
 *
 * PPAP's kernel clock is UTC-only — no timezone conversion happens
 * anywhere, so "date" always prints UTC and there's no TZ handling.
 * If you need local time, subtract your offset by hand.
 *
 * `-s EPOCH` takes a plain decimal integer (seconds).  Human-readable
 * stamp parsing (`-s "2026-04-22 12:00:00"`) is deferred until a
 * strptime-equivalent lands in uclib.
 */

#include "lib/uclib.h"

static int use_color = 1;
#define C(seq) (use_color ? (seq) : "")
#define C_RST   C("\033[0m")
#define C_BOLD  C("\033[1m")
#define C_DIM   C("\033[2m")
#define C_CYAN  C("\033[36m")

static void put_2digit(unsigned v) {
  char s[2];
  s[0] = (char)('0' + (v / 10) % 10);
  s[1] = (char)('0' + v % 10);
  write(1, s, 2);
}

static void put_4digit(unsigned v) {
  char s[4];
  s[0] = (char)('0' + (v / 1000) % 10);
  s[1] = (char)('0' + (v / 100) % 10);
  s[2] = (char)('0' + (v / 10) % 10);
  s[3] = (char)('0' + v % 10);
  write(1, s, 4);
}

static void print_now(void) {
  long ts[2];
  if (clock_gettime(0 /* CLOCK_REALTIME */, ts) != 0) {
    fputs("date: clock_gettime failed\n", stderr);
    _exit(1);
  }
  struct uc_tm t;
  uc_gmtime((uint32_t)ts[0], &t);

  fputs(C_BOLD, stdout);
  put_4digit((unsigned)t.year);
  fputs(C_RST, stdout);
  fputs(C_DIM, stdout);
  putchar('-');
  fputs(C_RST, stdout);
  put_2digit((unsigned)t.mon);
  fputs(C_DIM, stdout);
  putchar('-');
  fputs(C_RST, stdout);
  put_2digit((unsigned)t.mday);
  putchar(' ');
  fputs(C_CYAN, stdout);
  put_2digit((unsigned)t.hour);
  fputs(C_DIM, stdout);
  putchar(':');
  fputs(C_RST, stdout);
  fputs(C_CYAN, stdout);
  put_2digit((unsigned)t.min);
  fputs(C_DIM, stdout);
  putchar(':');
  fputs(C_RST, stdout);
  fputs(C_CYAN, stdout);
  put_2digit((unsigned)t.sec);
  fputs(C_RST, stdout);
  fputs(C_DIM, stdout);
  fputs(" UTC", stdout);
  fputs(C_RST, stdout);
  putchar('\n');
}

static int set_time(const char *s) {
  uint32_t epoch;
  if (uc_parse_u32(s, &epoch) != 0) {
    fputs("date: -s argument must be a positive decimal integer\n", stderr);
    return 1;
  }
  long tv[2] = {(long)epoch, 0};
  if (settimeofday(tv, 0) != 0) {
    fputs("date: settimeofday failed\n", stderr);
    return 1;
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
    if (strcmp(argv[argi], "--help") == 0) {
      fputs(
          "Usage: date [--no-color]\n"
          "       date -s EPOCH_SECONDS\n"
          "  No arg  Print current UTC time\n"
          "  -s      Set clock to EPOCH_SECONDS (plain decimal)\n", stdout);
      return 0;
    }
    if (strcmp(argv[argi], "--no-color") == 0) {
      use_color = 0;
      argi++;
      continue;
    }
    if (strcmp(argv[argi], "-s") == 0) {
      if (argi + 1 >= argc) {
        fputs("date: -s needs an argument\n", stderr);
        return 1;
      }
      return set_time(argv[argi + 1]);
    }
    fputs("date: unknown option: ", stderr);
    fputs(argv[argi], stderr);
    fputs("\n", stderr);
    return 1;
  }

  if (argi != argc) {
    fputs("date: unexpected argument: ", stderr);
    fputs(argv[argi], stderr);
    fputs("\n", stderr);
    return 1;
  }

  print_now();
  return 0;
}
