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
    if (uc_strcmp(argv[argi], "--help") == 0) {
      uc_puts(
          "Usage: chmod [--no-color] OCTAL-MODE FILE...\n"
          "  OCTAL-MODE is a 3- or 4-digit octal number (e.g. 755, 0644).\n"
          "  Symbolic modes (u+x, etc.) are not supported.\n");
      return 0;
    }
    if (uc_strcmp(argv[argi], "--no-color") == 0) {
      use_color = 0;
      argi++;
      continue;
    }
    /* A leading "-" that is NOT a known flag — but starts with a digit
     * — would mean "-0644" style (negative?), which POSIX chmod
     * doesn't support either.  Treat as unknown flag. */
    uc_eputs("chmod: unknown option: ");
    uc_eputs(argv[argi]);
    uc_eputs("\n");
    return 1;
  }

  if (argi >= argc) {
    uc_eputs("chmod: missing mode argument\n");
    return 1;
  }
  const char *mode_str = argv[argi++];

  uint32_t mode;
  if (parse_octal(mode_str, &mode) != 0) {
    uc_eputs(C_RED);
    uc_eputs("chmod: invalid mode: ");
    uc_eputs(C_RST);
    uc_eputs(mode_str);
    uc_eputs(" (must be 3- or 4-digit octal, 0..7777)\n");
    return 1;
  }

  if (argi >= argc) {
    uc_eputs("chmod: missing file operand\n");
    return 1;
  }

  int rc = 0;
  for (; argi < argc; argi++) {
    if (chmod(argv[argi], (int)mode) < 0) {
      uc_eputs(C_RED);
      uc_eputs("chmod: cannot change mode: ");
      uc_eputs(C_RST);
      uc_eputs(argv[argi]);
      uc_eputs("\n");
      rc = 1;
    }
  }
  return rc;
}
