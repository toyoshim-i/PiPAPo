/*
 * sleep.c — pause for SECONDS seconds
 *
 * Usage: sleep SECONDS [--help]
 *
 * SECONDS must be a non-negative integer.  Suffixes (s/m/h/d) and
 * fractional seconds are not supported — keep it minimal.
 */

#include "common/time.h"
#include "lib/uclib.h"

int main(int argc, char *argv[]) {
  int argi = 1;

  if (argi < argc && strcmp(argv[argi], "--help") == 0) {
    fputs("Usage: sleep SECONDS\n", stdout);
    return 0;
  }

  if (argi >= argc) {
    fputs("sleep: missing operand\n", stderr);
    return 1;
  }

  const char *s = argv[argi];
  uint32_t secs;
  if (uc_parse_u32(s, &secs) < 0) {
    fputs("sleep: invalid time interval: ", stderr);
    fputs(s, stderr);
    fputs("\n", stderr);
    return 1;
  }

  struct timespec req = {(long)secs, 0};
  if (nanosleep(&req, (void *)0) < 0) return 1;
  return 0;
}
