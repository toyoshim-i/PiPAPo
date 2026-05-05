/*
 * sort.c — sort lines of text
 *
 * Usage: sort [OPTION...] [file ...]
 *   -r  reverse the result
 *   -n  numeric sort (compare leading integers, then string for ties)
 *   -u  drop adjacent duplicates after sorting
 *   -f  fold lower case to upper case for the comparison
 *
 * With no file (or "-"), reads stdin.  All input is held in memory at
 * once: the heap pool is 4 KB, so the upper bound is roughly that
 * (minus per-allocation metadata) for combined input bytes plus the
 * line index.  Larger inputs fail with "out of memory".
 *
 * Exit status: 0 on success, 1 on read error or out of memory.
 */

#include "lib/uclib.h"

#define HEAP_POOL_SIZE 4096

#define F_R 0x01
#define F_N 0x02
#define F_U 0x04
#define F_F 0x08

static char heap_pool[HEAP_POOL_SIZE];
static int flags;

typedef struct {
  char *text;
  int len;
} line_t;

static int fold(int c) {
  if (c >= 'a' && c <= 'z') return c - ('a' - 'A');
  return c;
}

/* Skip leading whitespace, then parse a signed integer prefix.  Out-
 * parameter `consumed` reports how many characters were eaten (0 if no
 * digits found, leaving *out = 0). */
static long parse_leading_int(const char *s, int len, int *consumed) {
  int i = 0;
  while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
  int neg = 0;
  if (i < len && (s[i] == '-' || s[i] == '+')) {
    if (s[i] == '-') neg = 1;
    i++;
  }
  int start = i;
  long v = 0;
  while (i < len && s[i] >= '0' && s[i] <= '9') {
    v = v * 10 + (s[i] - '0');
    i++;
  }
  if (i == start) {
    *consumed = 0;
    return 0;
  }
  *consumed = i;
  return neg ? -v : v;
}

static int compare_strings(const char *a, int alen, const char *b, int blen) {
  int n = alen < blen ? alen : blen;
  if (flags & F_F) {
    for (int i = 0; i < n; i++) {
      int ca = fold((unsigned char)a[i]);
      int cb = fold((unsigned char)b[i]);
      if (ca != cb) return ca - cb;
    }
  } else {
    int c = memcmp(a, b, n);
    if (c != 0) return c;
  }
  return alen - blen;
}

static int compare_lines(const line_t *a, const line_t *b) {
  if (flags & F_N) {
    int ca, cb;
    long va = parse_leading_int(a->text, a->len, &ca);
    long vb = parse_leading_int(b->text, b->len, &cb);
    if (ca || cb) {
      if (va != vb) return (va < vb) ? -1 : 1;
    }
  }
  return compare_strings(a->text, a->len, b->text, b->len);
}

/* Plain insertion sort: small-N inputs only (we cap at the heap pool
 * size, ~few hundred lines tops), and avoiding qsort/recursion keeps
 * the binary tiny. */
static void sort_lines(line_t *arr, int n) {
  for (int i = 1; i < n; i++) {
    line_t key = arr[i];
    int j = i - 1;
    while (j >= 0 && compare_lines(&arr[j], &key) > 0) {
      arr[j + 1] = arr[j];
      j--;
    }
    arr[j + 1] = key;
  }
}

/* Append `n` bytes from `src` to *buf (malloc'd, *cap bytes), growing
 * by doubling.  Returns 0 on success, -1 on OOM. */
static int append_bytes(char **buf, int *len, int *cap, const char *src,
                        int n) {
  if (*len + n > *cap) {
    int new_cap = *cap ? *cap * 2 : 256;
    while (new_cap < *len + n) new_cap *= 2;
    char *nb = malloc((size_t)new_cap);
    if (!nb) return -1;
    if (*buf) {
      memcpy(nb, *buf, *len);
      free(*buf);
    }
    *buf = nb;
    *cap = new_cap;
  }
  memcpy(*buf + *len, src, n);
  *len += n;
  return 0;
}

/* Slurp all of `fd` into the growing buffer.  Returns 0 on success,
 * -1 on read error, -2 on out-of-memory. */
static int slurp_fd(int fd, char **buf, int *len, int *cap) {
  char chunk[256];
  ssize_t n;
  while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
    if (append_bytes(buf, len, cap, chunk, (int)n) < 0) return -2;
  }
  return (n < 0) ? -1 : 0;
}

int main(int argc, char *argv[]) {
  uc_heap_init(heap_pool, sizeof(heap_pool));

  int argi = 1;
  while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0' &&
         strcmp(argv[argi], "-") != 0) {
    if (strcmp(argv[argi], "--") == 0) {
      argi++;
      break;
    }
    if (strcmp(argv[argi], "--help") == 0) {
      uc_puts(
          "Usage: sort [-rnuf] [file ...]\n"
          "  -r  Reverse output order\n"
          "  -n  Numeric compare (leading integer)\n"
          "  -u  Drop adjacent duplicates\n"
          "  -f  Fold case for compare\n"
          "  -   Read from stdin\n");
      return 0;
    }
    const char *p = argv[argi] + 1;
    while (*p) {
      switch (*p) {
        case 'r': flags |= F_R; break;
        case 'n': flags |= F_N; break;
        case 'u': flags |= F_U; break;
        case 'f': flags |= F_F; break;
        default:
          uc_eputs("sort: unknown option: -");
          putchar(*p);
          uc_eputs("\n");
          return 1;
      }
      p++;
    }
    argi++;
  }

  /* Read all input into one growing buffer. */
  char *buf = NULL;
  int buf_len = 0;
  int buf_cap = 0;

  if (argi >= argc) {
    int rc = slurp_fd(0, &buf, &buf_len, &buf_cap);
    if (rc == -1) {
      uc_eputs("sort: read error on stdin\n");
      return 1;
    }
    if (rc == -2) {
      uc_eputs("sort: out of memory\n");
      return 1;
    }
  } else {
    for (int i = argi; i < argc; i++) {
      int fd;
      const char *name = argv[i];
      if (strcmp(name, "-") == 0) {
        fd = 0;
      } else {
        fd = open(name, O_RDONLY, 0);
        if (fd < 0) {
          uc_eputs("sort: ");
          uc_eputs(name);
          uc_eputs(": No such file or directory\n");
          return 1;
        }
      }
      int rc = slurp_fd(fd, &buf, &buf_len, &buf_cap);
      if (fd != 0) close(fd);
      if (rc == -1) {
        uc_eputs("sort: read error\n");
        return 1;
      }
      if (rc == -2) {
        uc_eputs("sort: out of memory\n");
        return 1;
      }
    }
  }

  /* Count lines first so we can size the index. */
  int nlines = 0;
  for (int i = 0; i < buf_len; i++) {
    if (buf[i] == '\n') nlines++;
  }
  int trailing = (buf_len > 0 && buf[buf_len - 1] != '\n') ? 1 : 0;
  if (trailing) nlines++;

  if (nlines == 0) return 0;

  line_t *lines = malloc((size_t)nlines * sizeof(line_t));
  if (!lines) {
    uc_eputs("sort: out of memory\n");
    return 1;
  }

  int li = 0;
  int start = 0;
  for (int i = 0; i < buf_len; i++) {
    if (buf[i] == '\n') {
      lines[li].text = buf + start;
      lines[li].len = i - start;
      li++;
      start = i + 1;
    }
  }
  if (trailing) {
    lines[li].text = buf + start;
    lines[li].len = buf_len - start;
  }

  sort_lines(lines, nlines);

  int step = (flags & F_R) ? -1 : 1;
  int begin = (flags & F_R) ? nlines - 1 : 0;
  int end = (flags & F_R) ? -1 : nlines;

  const char *prev = NULL;
  int prev_len = 0;
  for (int i = begin; i != end; i += step) {
    if ((flags & F_U) && prev) {
      if (lines[i].len == prev_len &&
          compare_strings(lines[i].text, lines[i].len, prev, prev_len) == 0) {
        continue;
      }
    }
    write(1, lines[i].text, (size_t)lines[i].len);
    putchar('\n');
    prev = lines[i].text;
    prev_len = lines[i].len;
  }

  free(lines);
  free(buf);
  return 0;
}
