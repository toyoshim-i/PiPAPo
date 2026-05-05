/*
 * grep.c — print lines matching a fixed-string pattern
 *
 * Usage: grep [OPTION...] PATTERN [file ...]
 *   -n  prefix each match with its line number
 *   -i  ignore case
 *   -v  invert match (print non-matching lines)
 *   -c  print only the count of matching lines per file
 *   -q  quiet — exit status only, no output
 *   -h  suppress filename prefix when scanning multiple files
 *   -H  always prefix filename, even with a single file
 *   -F  fixed-string match (always on; accepted for compatibility)
 *
 * With no file (or "-"), reads stdin.  Lines longer than 1024 bytes
 * are silently truncated for matching purposes; the truncated text is
 * what gets printed.
 *
 * Exit status: 0 if any line matched, 1 if none, 2 on error.
 */

#include "lib/uclib.h"

#define LINE_MAX 1024
#define READ_CHUNK 512

#define F_N 0x01
#define F_I 0x02
#define F_V 0x04
#define F_C 0x08
#define F_Q 0x10
#define F_H 0x20  /* suppress filename */
#define F_BIGH 0x40  /* force filename */

static int flags;
static const char *pattern;
static int pattern_len;

static int to_lower(int c) {
  if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
  return c;
}

/* Return 1 if `pattern` occurs in `line` (length len), else 0.
 * Honours F_I for case-insensitive search. */
static int line_matches(const char *line, int len) {
  if (pattern_len == 0) return 1;
  if (pattern_len > len) return 0;
  int last = len - pattern_len;
  if (flags & F_I) {
    for (int i = 0; i <= last; i++) {
      int j = 0;
      while (j < pattern_len &&
             to_lower((unsigned char)line[i + j]) ==
                 to_lower((unsigned char)pattern[j])) {
        j++;
      }
      if (j == pattern_len) return 1;
    }
  } else {
    char first = pattern[0];
    for (int i = 0; i <= last; i++) {
      if (line[i] != first) continue;
      if (memcmp(line + i, pattern, pattern_len) == 0) return 1;
    }
  }
  return 0;
}

/* Stream `fd`, applying the match to each line.  `name` (may be NULL)
 * is used as the filename prefix when emitting matches.  `print_name`
 * controls whether filenames are shown.  Updates *count with the number
 * of matching lines.  Returns 1 on read error, else 0. */
static int grep_fd(int fd, const char *name, int print_name,
                   uint32_t *count) {
  char line[LINE_MAX];
  char chunk[READ_CHUNK];
  int line_len = 0;
  uint32_t lineno = 0;
  ssize_t n;
  *count = 0;

  while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
    for (ssize_t i = 0; i < n; i++) {
      char c = chunk[i];
      if (c == '\n') {
        lineno++;
        int matched = line_matches(line, line_len);
        if (flags & F_V) matched = !matched;
        if (matched) {
          (*count)++;
          if (!(flags & (F_C | F_Q))) {
            if (print_name) {
              uc_puts(name);
              putchar(':');
            }
            if (flags & F_N) printf("%u:", (unsigned)lineno);
            write(1, line, (size_t)line_len);
            putchar('\n');
          }
        }
        line_len = 0;
      } else if (line_len < LINE_MAX) {
        line[line_len++] = c;
      }
    }
  }
  /* Trailing data without a terminating newline still counts as a line. */
  if (line_len > 0) {
    lineno++;
    int matched = line_matches(line, line_len);
    if (flags & F_V) matched = !matched;
    if (matched) {
      (*count)++;
      if (!(flags & (F_C | F_Q))) {
        if (print_name) {
          uc_puts(name);
          putchar(':');
        }
        if (flags & F_N) printf("%u:", (unsigned)lineno);
        write(1, line, (size_t)line_len);
        putchar('\n');
      }
    }
  }
  return (n < 0) ? 1 : 0;
}

static int parse_options(int argc, char *argv[]) {
  int argi = 1;
  while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0' &&
         strcmp(argv[argi], "-") != 0) {
    if (strcmp(argv[argi], "--") == 0) return argi + 1;
    if (strcmp(argv[argi], "--help") == 0) {
      uc_puts(
          "Usage: grep [-nivcqhHF] PATTERN [file ...]\n"
          "  -n  Prefix each line with line number\n"
          "  -i  Case-insensitive match\n"
          "  -v  Invert match\n"
          "  -c  Print count of matches per file\n"
          "  -q  Quiet — exit status only\n"
          "  -h  Never print filename prefix\n"
          "  -H  Always print filename prefix\n"
          "  -F  Fixed string (default; accepted for compat)\n"
          "  -   Read from stdin\n");
      return -2;
    }
    const char *p = argv[argi] + 1;
    while (*p) {
      switch (*p) {
        case 'n': flags |= F_N; break;
        case 'i': flags |= F_I; break;
        case 'v': flags |= F_V; break;
        case 'c': flags |= F_C; break;
        case 'q': flags |= F_Q; break;
        case 'h': flags |= F_H; break;
        case 'H': flags |= F_BIGH; break;
        case 'F': break;
        default:
          uc_eputs("grep: unknown option: -");
          putchar(*p);
          uc_eputs("\n");
          return -1;
      }
      p++;
    }
    argi++;
  }
  return argi;
}

int main(int argc, char *argv[]) {
  int argi = parse_options(argc, argv);
  if (argi == -1) return 2;
  if (argi == -2) return 0;

  if (argi >= argc) {
    uc_eputs("grep: missing pattern\n");
    return 2;
  }
  pattern = argv[argi++];
  pattern_len = strlen(pattern);

  int any_matched = 0;
  int rc = 0;
  int nfiles = argc - argi;
  int print_name;

  if (flags & F_H) {
    print_name = 0;
  } else if (flags & F_BIGH) {
    print_name = 1;
  } else {
    print_name = (nfiles > 1);
  }

  if (nfiles == 0) {
    uint32_t count;
    if (grep_fd(0, "(stdin)", print_name, &count)) rc = 2;
    if (flags & F_C) printf("%u\n", (unsigned)count);
    if (count > 0) any_matched = 1;
  } else {
    for (int i = argi; i < argc; i++) {
      int fd;
      const char *name = argv[i];
      if (strcmp(name, "-") == 0) {
        fd = 0;
      } else {
        fd = open(name, O_RDONLY, 0);
        if (fd < 0) {
          uc_eputs("grep: ");
          uc_eputs(name);
          uc_eputs(": No such file or directory\n");
          rc = 2;
          continue;
        }
      }
      uint32_t count;
      if (grep_fd(fd, name, print_name, &count)) rc = 2;
      if (flags & F_C) {
        if (print_name) {
          uc_puts(name);
          putchar(':');
        }
        printf("%u\n", (unsigned)count);
      }
      if (count > 0) any_matched = 1;
      if (fd != 0) close(fd);
    }
  }
  if (rc != 0) return rc;
  return any_matched ? 0 : 1;
}
