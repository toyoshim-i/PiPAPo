/*
 * chmod.c — change file permission bits
 *
 * Usage: chmod OCTAL-MODE FILE...
 *
 * Only absolute-octal mode is supported (e.g. "755", "0644", "4755").
 * Symbolic mode (u+x, g-w, a=rx, …) is not parsed — busybox chmod is
 * still around if that's needed.  The octal value is masked to the
 * low 12 bits (07777) so the setuid / setgid / sticky bits pass
 * through even though PPAP has no real permission model yet —
 * stat64 will report them back faithfully, which is good enough for
 * apps that round-trip.
 */

#include "lib/uclib.h"

static int use_color = 1;
#define C(seq) (use_color ? (seq) : "")
#define C_RST C("\033[0m")
#define C_RED C("\033[31m")

static int parse_octal(const char *s, uint32_t *out) {
  uint32_t v = 0;
  int seen = 0;
  while (*s) {
    if (*s < '0' || *s > '7') return -1;
    v = (v << 3) | (uint32_t)(*s - '0');
    seen = 1;
    s++;
  }
  if (!seen) return -1;
  if (v > 07777u) return -1;
  *out = v;
  return 0;
}

int main(int argc, char *argv[]) {
  int argi = 1;

  while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
    if (strcmp(argv[argi], "--help") == 0) {
      fputs(
          "Usage: chmod [--no-color] OCTAL-MODE FILE...\n"
          "  OCTAL-MODE is a 3- or 4-digit octal number (e.g. 755, 0644).\n"
          "  Symbolic modes (u+x, etc.) are not supported.\n", stdout);
      return 0;
    }
    if (strcmp(argv[argi], "--no-color") == 0) {
      use_color = 0;
      argi++;
      continue;
    }
    /* A leading "-" that is NOT a known flag — but starts with a digit
     * — would mean "-0644" style (negative?), which POSIX chmod
     * doesn't support either.  Treat as unknown flag. */
    fputs("chmod: unknown option: ", stderr);
    fputs(argv[argi], stderr);
    fputs("\n", stderr);
    return 1;
  }

  if (argi >= argc) {
    fputs("chmod: missing mode argument\n", stderr);
    return 1;
  }
  const char *mode_str = argv[argi++];

  uint32_t mode;
  if (parse_octal(mode_str, &mode) != 0) {
    fputs(C_RED, stderr);
    fputs("chmod: invalid mode: ", stderr);
    fputs(C_RST, stderr);
    fputs(mode_str, stderr);
    fputs(" (must be 3- or 4-digit octal, 0..7777)\n", stderr);
    return 1;
  }

  if (argi >= argc) {
    fputs("chmod: missing file operand\n", stderr);
    return 1;
  }

  int rc = 0;
  for (; argi < argc; argi++) {
    if (chmod(argv[argi], (int)mode) < 0) {
      fputs(C_RED, stderr);
      fputs("chmod: cannot change mode: ", stderr);
      fputs(C_RST, stderr);
      fputs(argv[argi], stderr);
      fputs("\n", stderr);
      rc = 1;
    }
  }
  return rc;
}
