/*
 * sed.c — stream editor (subset of POSIX sed)
 *
 * Usage: sed [-n] [-e SCRIPT]... [-f SCRIPTFILE]... [SCRIPT] [file ...]
 *   -n  suppress default print of pattern space
 *   -e SCRIPT     append SCRIPT to the program (may repeat)
 *   -f SCRIPTFILE read SCRIPT from file (may repeat)
 *
 * Commands implemented:
 *   s/RE/REPL/[g][N][p]   substitute (BRE; & = whole match; \1..\9 backref)
 *   d                     delete pattern space and start next cycle
 *   p                     print pattern space
 *   =                     print current line number
 *   q                     quit (still prints pattern space unless -n)
 *   n                     print + read next line into pattern space
 *   N                     append next line (with embedded \n) to pattern space
 *   #...                  comment line
 *
 * Addresses:
 *   N        line N
 *   $        last line
 *   /RE/     regex match against pattern space
 *   addr1,addr2   range from when addr1 matches until addr2 matches
 *   (no addr)     every line
 *
 * Regex: BRE only.  . * ^ $ [...] [^...] [a-z] \(...\) \1..\9 and
 * \. \* \[ \] \\ \/ escapes.  No ?, +, {n,m}, alternation.  Anchors
 * are positional only ($ at end, ^ at start).
 *
 * Limits (raise in source if you hit them):
 *   32 commands per program
 *   16 regexes per program
 *   4 KB pattern space
 *   4 KB regex bytecode + replacement string pool
 *
 * Exit status: 0 on success, 1 on parse / runtime error.
 */

#include "lib/uclib.h"

/* ── Limits ──────────────────────────────────────────────────────────── */

#define MAX_CMDS 32
#define MAX_REGEX 16
#define MAX_GROUPS 9
#define PATTERN_SPACE_SIZE 4096
#define POOL_SIZE 4096
#define MAX_RE_OPS 256

/* ── Regex bytecode ──────────────────────────────────────────────────── */

enum re_op {
  RE_END = 0,
  RE_LIT,        /* match exactly arg */
  RE_ANY,        /* match any char (incl. nothing matches \n is ok in sed) */
  RE_CLASS,      /* arg = byte offset into class_table for 32-byte bitmap */
  RE_BOL,        /* zero-width: at start of line */
  RE_EOL,        /* zero-width: at end of line */
  RE_GROUP_OPEN, /* arg = group id */
  RE_GROUP_CLOSE,
  RE_BACKREF, /* arg = group id */
};

typedef struct {
  uint8_t op;      /* enum re_op */
  uint8_t arg;     /* per-op data */
  uint8_t starred; /* applies STAR closure to this atom */
} re_inst_t;

typedef struct {
  re_inst_t code[MAX_RE_OPS];
  int n_code;
  /* Class bitmaps: each class is 32 bytes (256 bits).  Indexed by
   * inst.arg (which is a class slot ID, 0..). */
  uint8_t classes[8][32];
  int n_classes;
  int n_groups;
} regex_t;

/* Match-time capture: start/end byte offsets into the input string, or
 * -1 if the group did not participate. */
typedef struct {
  int start;
  int end;
} capture_t;

/* ── Compiled program ────────────────────────────────────────────────── */

enum addr_type {
  ADDR_NONE = 0,
  ADDR_NUM,
  ADDR_DOLLAR,
  ADDR_REGEX,
};

typedef struct {
  uint8_t type;     /* enum addr_type */
  int num;          /* for ADDR_NUM */
  int regex_idx;    /* for ADDR_REGEX, index into regex_pool */
} address_t;

enum cmd_type {
  CMD_S = 's',
  CMD_D = 'd',
  CMD_P = 'p',
  CMD_EQ = '=',
  CMD_Q = 'q',
  CMD_N_LOWER = 'n',
  CMD_N_UPPER = 'N',
};

typedef struct {
  uint8_t type;
  address_t a1;
  address_t a2;
  int in_range;       /* runtime flag for two-address commands */

  /* For CMD_S only: */
  int regex_idx;        /* pattern slot */
  const char *replacement;  /* into pool */
  uint8_t s_global;     /* /g */
  uint8_t s_print;      /* /p */
  int s_nth;            /* /N — only Nth match (0 = no N) */
} command_t;

static command_t cmds[MAX_CMDS];
static int n_cmds;
static regex_t regex_pool[MAX_REGEX];
static int n_regex;
static char pool[POOL_SIZE];
static int pool_used;
static int n_flag;            /* -n: suppress default print */
static int last_line_seen;    /* set when input source signals EOF */

/* ── Pool helpers ────────────────────────────────────────────────────── */

static char *pool_strndup(const char *s, int n) {
  if (pool_used + n + 1 > POOL_SIZE) return NULL;
  char *out = pool + pool_used;
  uc_memcpy(out, s, n);
  out[n] = '\0';
  pool_used += n + 1;
  return out;
}

/* ── Regex compiler ──────────────────────────────────────────────────── */

static int re_emit(regex_t *re, uint8_t op, uint8_t arg) {
  if (re->n_code >= MAX_RE_OPS) return -1;
  re->code[re->n_code].op = op;
  re->code[re->n_code].arg = arg;
  re->code[re->n_code].starred = 0;
  return re->n_code++;
}

static void class_set(uint8_t *bm, int b) { bm[b >> 3] |= (uint8_t)(1u << (b & 7)); }

/* Compile [...] starting at *p (which points just past '[').  Advances
 * *p past the closing ']'.  Returns 0 on success, -1 on error. */
static int compile_class(regex_t *re, const char **p) {
  if (re->n_classes >= 8) return -1;
  uint8_t *bm = re->classes[re->n_classes];
  uc_memset(bm, 0, 32);
  int negate = 0;
  if (**p == '^') {
    negate = 1;
    (*p)++;
  }
  /* First char (or ']' as first char) is always literal. */
  if (**p == '\0') return -1;
  int prev = -1;
  int first = 1;
  while (**p && (**p != ']' || first)) {
    int c = (unsigned char)**p;
    (*p)++;
    if (**p == '-' && (*p)[1] != ']' && (*p)[1] != '\0') {
      (*p)++;
      int hi = (unsigned char)**p;
      (*p)++;
      for (int i = c; i <= hi; i++) class_set(bm, i);
      prev = -1;
    } else {
      class_set(bm, c);
      prev = c;
    }
    first = 0;
    (void)prev;
  }
  if (**p != ']') return -1;
  (*p)++;
  if (negate) {
    for (int i = 0; i < 32; i++) bm[i] = (uint8_t)~bm[i];
  }
  int idx = re->n_classes++;
  if (re_emit(re, RE_CLASS, (uint8_t)idx) < 0) return -1;
  return 0;
}

/* Compile a BRE pattern.  Returns 0 on success, -1 on error.
 * Pattern is delimited by `delim` (typically '/' for sed regexes,
 * or '\0' for raw patterns from /addr/).  The closing delimiter is
 * consumed; `*p` ends pointing at it (caller advances if needed).
 *
 * Returns the index into regex_pool. */
static int compile_regex(const char **p, char delim) {
  if (n_regex >= MAX_REGEX) return -1;
  regex_t *re = &regex_pool[n_regex];
  re->n_code = 0;
  re->n_classes = 0;
  re->n_groups = 0;
  int group_stack[MAX_GROUPS];
  int group_depth = 0;
  int last_atom = -1;

  while (**p && **p != delim) {
    char c = **p;
    (*p)++;
    int emitted;
    if (c == '\\') {
      char nc = **p;
      if (nc == '\0') return -1;
      (*p)++;
      if (nc == '(') {
        if (re->n_groups >= MAX_GROUPS) return -1;
        if (group_depth >= MAX_GROUPS) return -1;
        int gid = re->n_groups++;
        emitted = re_emit(re, RE_GROUP_OPEN, (uint8_t)gid);
        group_stack[group_depth++] = gid;
        last_atom = -1; /* groups can't be starred in BRE — sed treats
                           \(...\)* as star on the group */
        if (emitted < 0) return -1;
        continue;
      }
      if (nc == ')') {
        if (group_depth == 0) return -1;
        int gid = group_stack[--group_depth];
        emitted = re_emit(re, RE_GROUP_CLOSE, (uint8_t)gid);
        if (emitted < 0) return -1;
        last_atom = emitted;
        continue;
      }
      if (nc >= '1' && nc <= '9') {
        emitted = re_emit(re, RE_BACKREF, (uint8_t)(nc - '0'));
        if (emitted < 0) return -1;
        last_atom = emitted;
        continue;
      }
      /* Escaped literal */
      emitted = re_emit(re, RE_LIT, (uint8_t)nc);
    } else if (c == '.') {
      emitted = re_emit(re, RE_ANY, 0);
    } else if (c == '^' && re->n_code == 0) {
      emitted = re_emit(re, RE_BOL, 0);
      last_atom = -1;
      if (emitted < 0) return -1;
      continue;
    } else if (c == '$' && (**p == '\0' || **p == delim)) {
      emitted = re_emit(re, RE_EOL, 0);
      last_atom = -1;
      if (emitted < 0) return -1;
      continue;
    } else if (c == '[') {
      int before = re->n_code;
      if (compile_class(re, p) < 0) return -1;
      emitted = before;
    } else if (c == '*') {
      if (last_atom < 0) {
        /* `*` at start or after non-atom: treat as literal */
        emitted = re_emit(re, RE_LIT, (uint8_t)'*');
      } else {
        re->code[last_atom].starred = 1;
        last_atom = -1;
        continue;
      }
    } else {
      emitted = re_emit(re, RE_LIT, (uint8_t)c);
    }
    if (emitted < 0) return -1;
    last_atom = emitted;
  }
  if (group_depth != 0) return -1;
  if (re_emit(re, RE_END, 0) < 0) return -1;
  return n_regex++;
}

/* ── Regex matcher ───────────────────────────────────────────────────── */

/* Recursive backtracker.  re_match tries to match starting at code
 * position `pc` and string position `sp`.  Returns the new sp on
 * success, -1 on failure. */
static int try_match(const regex_t *re, int pc, const char *s, int sp,
                     int slen, capture_t *caps);

static int class_test(const uint8_t *bm, unsigned char c) {
  return (bm[c >> 3] >> (c & 7)) & 1;
}

static int atom_match_one(const regex_t *re, int pc, const char *s, int sp,
                          int slen, capture_t *caps) {
  const re_inst_t *ip = &re->code[pc];
  switch (ip->op) {
    case RE_LIT:
      if (sp < slen && (unsigned char)s[sp] == ip->arg) return sp + 1;
      return -1;
    case RE_ANY:
      if (sp < slen && s[sp] != '\n') return sp + 1;
      return -1;
    case RE_CLASS:
      if (sp < slen && class_test(re->classes[ip->arg], (unsigned char)s[sp]))
        return sp + 1;
      return -1;
    case RE_BACKREF: {
      int g = ip->arg - 1;
      if (g < 0 || g >= MAX_GROUPS) return -1;
      if (caps[g].start < 0 || caps[g].end < 0) return -1;
      int n = caps[g].end - caps[g].start;
      if (sp + n > slen) return -1;
      if (uc_memcmp(s + sp, s + caps[g].start, n) != 0) return -1;
      return sp + n;
    }
    default:
      return -1;
  }
}

static int try_match(const regex_t *re, int pc, const char *s, int sp,
                     int slen, capture_t *caps) {
  for (;;) {
    const re_inst_t *ip = &re->code[pc];
    switch (ip->op) {
      case RE_END:
        return sp;
      case RE_BOL:
        if (sp != 0) return -1;
        pc++;
        continue;
      case RE_EOL:
        if (sp != slen) return -1;
        pc++;
        continue;
      case RE_GROUP_OPEN: {
        int saved = caps[ip->arg].start;
        caps[ip->arg].start = sp;
        int r = try_match(re, pc + 1, s, sp, slen, caps);
        if (r >= 0) return r;
        caps[ip->arg].start = saved;
        return -1;
      }
      case RE_GROUP_CLOSE: {
        int saved = caps[ip->arg].end;
        caps[ip->arg].end = sp;
        int r = try_match(re, pc + 1, s, sp, slen, caps);
        if (r >= 0) return r;
        caps[ip->arg].end = saved;
        return -1;
      }
      default: {
        if (ip->starred) {
          /* Greedy: try N..0 matches, taking the longest that leads to
           * a full match for the rest.  Build a list of valid step-end
           * positions, then try them in reverse order. */
          int positions[256];
          int n = 0;
          positions[n++] = sp;
          int cur = sp;
          while (n < 256) {
            int next = atom_match_one(re, pc, s, cur, slen, caps);
            if (next < 0 || next == cur) break;
            positions[n++] = next;
            cur = next;
          }
          for (int i = n - 1; i >= 0; i--) {
            int r = try_match(re, pc + 1, s, positions[i], slen, caps);
            if (r >= 0) return r;
          }
          return -1;
        }
        int next = atom_match_one(re, pc, s, sp, slen, caps);
        if (next < 0) return -1;
        sp = next;
        pc++;
        continue;
      }
    }
  }
}

/* Search for `re` in s[0..slen-1].  Returns the start offset of the
 * earliest match, or -1 if no match.  Fills caps[].  caps must have
 * MAX_GROUPS entries; on entry every entry should be {-1,-1}. */
static int regex_search(const regex_t *re, const char *s, int slen,
                        capture_t *caps, int *match_end) {
  for (int i = 0; i <= slen; i++) {
    for (int g = 0; g < MAX_GROUPS; g++) caps[g].start = caps[g].end = -1;
    int r = try_match(re, 0, s, i, slen, caps);
    if (r >= 0) {
      *match_end = r;
      return i;
    }
    /* If pattern starts with ^, only one starting position is valid. */
    if (re->code[0].op == RE_BOL) return -1;
  }
  return -1;
}

/* ── Script parser ───────────────────────────────────────────────────── */

static int is_digit(int c) { return c >= '0' && c <= '9'; }

static int parse_address(const char **p, address_t *out) {
  out->type = ADDR_NONE;
  while (**p == ' ' || **p == '\t') (*p)++;
  if (**p == '\0') return 0;
  if (is_digit(**p)) {
    int v = 0;
    while (is_digit(**p)) {
      v = v * 10 + (**p - '0');
      (*p)++;
    }
    out->type = ADDR_NUM;
    out->num = v;
    return 1;
  }
  if (**p == '$') {
    (*p)++;
    out->type = ADDR_DOLLAR;
    return 1;
  }
  if (**p == '/') {
    (*p)++;
    int idx = compile_regex(p, '/');
    if (idx < 0 || **p != '/') return -1;
    (*p)++;
    out->type = ADDR_REGEX;
    out->regex_idx = idx;
    return 1;
  }
  return 0;
}

/* Parse one command from *p; advance *p past it (and past terminator
 * `;` or `\n`).  Returns 0 on success, -1 on error.  */
static int parse_command(const char **p) {
  while (**p == ' ' || **p == '\t' || **p == '\n' || **p == ';') (*p)++;
  if (**p == '\0') return 1; /* end */
  if (**p == '#') {
    /* Comment: skip to end-of-line */
    while (**p && **p != '\n') (*p)++;
    return 0;
  }
  if (n_cmds >= MAX_CMDS) return -1;
  command_t *c = &cmds[n_cmds];
  c->type = 0;
  c->a1.type = c->a2.type = ADDR_NONE;
  c->in_range = 0;

  int got1 = parse_address(p, &c->a1);
  if (got1 < 0) return -1;
  if (got1 && **p == ',') {
    (*p)++;
    int got2 = parse_address(p, &c->a2);
    if (got2 <= 0) return -1;
  }
  while (**p == ' ' || **p == '\t') (*p)++;

  char cmd = **p;
  if (cmd == '\0') return -1;
  (*p)++;
  c->type = (uint8_t)cmd;

  switch (cmd) {
    case CMD_D:
    case CMD_P:
    case CMD_EQ:
    case CMD_Q:
    case CMD_N_LOWER:
    case CMD_N_UPPER:
      break;
    case CMD_S: {
      char delim = **p;
      if (delim == '\0' || delim == '\n') return -1;
      (*p)++;
      int idx = compile_regex(p, delim);
      if (idx < 0 || **p != delim) return -1;
      c->regex_idx = idx;
      (*p)++;
      const char *rep_start = *p;
      while (**p && **p != delim && **p != '\n') {
        if (**p == '\\' && (*p)[1]) (*p)++;
        (*p)++;
      }
      if (**p != delim) return -1;
      int rep_len = (int)(*p - rep_start);
      char *rep = pool_strndup(rep_start, rep_len);
      if (!rep) return -1;
      c->replacement = rep;
      (*p)++;
      c->s_global = 0;
      c->s_print = 0;
      c->s_nth = 0;
      while (**p && **p != ';' && **p != '\n') {
        if (**p == 'g') {
          c->s_global = 1;
          (*p)++;
        } else if (**p == 'p') {
          c->s_print = 1;
          (*p)++;
        } else if (is_digit(**p)) {
          int v = 0;
          while (is_digit(**p)) {
            v = v * 10 + (**p - '0');
            (*p)++;
          }
          c->s_nth = v;
        } else {
          return -1;
        }
      }
      break;
    }
    default:
      return -1;
  }
  while (**p == ' ' || **p == '\t') (*p)++;
  if (**p == ';' || **p == '\n') (*p)++;
  n_cmds++;
  return 0;
}

static int parse_script(const char *src) {
  while (*src) {
    int r = parse_command(&src);
    if (r < 0) return -1;
    if (r == 1) break;
  }
  return 0;
}

/* ── Execute ─────────────────────────────────────────────────────────── */

static int addr_match_single(const address_t *a, int lineno,
                             const char *pspace, int plen) {
  switch (a->type) {
    case ADDR_NUM: return lineno == a->num;
    case ADDR_DOLLAR: return last_line_seen;
    case ADDR_REGEX: {
      capture_t caps[MAX_GROUPS];
      int end;
      return regex_search(&regex_pool[a->regex_idx], pspace, plen, caps,
                          &end) >= 0;
    }
  }
  return 0;
}

static int cmd_addr_match(command_t *c, int lineno, const char *pspace,
                          int plen) {
  if (c->a1.type == ADDR_NONE) return 1;
  if (c->a2.type == ADDR_NONE) {
    return addr_match_single(&c->a1, lineno, pspace, plen);
  }
  /* Two-address range */
  if (c->in_range) {
    int end = addr_match_single(&c->a2, lineno, pspace, plen);
    if (end) c->in_range = 0;
    return 1;
  }
  if (addr_match_single(&c->a1, lineno, pspace, plen)) {
    if (!addr_match_single(&c->a2, lineno, pspace, plen)) c->in_range = 1;
    return 1;
  }
  return 0;
}

/* Apply substitution.  Writes new pattern space (NUL-terminated) into
 * out (with len), reading from src.  Returns number of substitutions
 * performed, or -1 on overflow. */
static int do_subst(command_t *c, const char *src, int slen, char *out,
                    int out_max, int *out_len) {
  regex_t *re = &regex_pool[c->regex_idx];
  int oi = 0;
  int si = 0;
  int matches = 0;
  int subs = 0;
  while (si <= slen) {
    capture_t caps[MAX_GROUPS];
    int end;
    int start = -1;
    /* Search remainder of src starting at si. */
    {
      const char *base = src + si;
      int blen = slen - si;
      int s = regex_search(re, base, blen, caps, &end);
      if (s >= 0) {
        start = si + s;
        end += si;
        for (int g = 0; g < MAX_GROUPS; g++) {
          if (caps[g].start >= 0) caps[g].start += si;
          if (caps[g].end >= 0) caps[g].end += si;
        }
      }
    }
    if (start < 0) break;
    matches++;
    /* Copy literal up to start. */
    int pre = start - si;
    if (oi + pre > out_max) return -1;
    uc_memcpy(out + oi, src + si, pre);
    oi += pre;

    int do_sub = 1;
    if (c->s_nth > 0 && matches != c->s_nth && !c->s_global) do_sub = 0;
    if (c->s_nth > 0 && c->s_global && matches < c->s_nth) do_sub = 0;
    if (!c->s_global && c->s_nth == 0 && subs > 0) do_sub = 0;

    if (do_sub) {
      /* Emit replacement.  & = whole match, \1..\9 = group, \\ = \,
       * \n = newline, \X otherwise = X. */
      const char *r = c->replacement;
      for (; *r; r++) {
        if (*r == '&') {
          int n = end - start;
          if (oi + n > out_max) return -1;
          uc_memcpy(out + oi, src + start, n);
          oi += n;
        } else if (*r == '\\' && r[1]) {
          char nc = *(++r);
          if (nc >= '1' && nc <= '9') {
            int g = nc - '1';
            if (g < MAX_GROUPS && caps[g].start >= 0 && caps[g].end >= 0) {
              int n = caps[g].end - caps[g].start;
              if (oi + n > out_max) return -1;
              uc_memcpy(out + oi, src + caps[g].start, n);
              oi += n;
            }
          } else if (nc == 'n') {
            if (oi + 1 > out_max) return -1;
            out[oi++] = '\n';
          } else {
            if (oi + 1 > out_max) return -1;
            out[oi++] = nc;
          }
        } else {
          if (oi + 1 > out_max) return -1;
          out[oi++] = *r;
        }
      }
      subs++;
      si = end;
      /* Empty match: advance one char to avoid infinite loop. */
      if (start == end) {
        if (si < slen) {
          if (oi + 1 > out_max) return -1;
          out[oi++] = src[si++];
        } else {
          break;
        }
      }
    } else {
      /* Skip past match without substituting. */
      int n = end - start;
      if (oi + n > out_max) return -1;
      uc_memcpy(out + oi, src + start, n);
      oi += n;
      si = end;
      if (start == end) {
        if (si < slen) {
          if (oi + 1 > out_max) return -1;
          out[oi++] = src[si++];
        } else {
          break;
        }
      }
    }
    if (!c->s_global && c->s_nth == 0 && subs > 0) {
      /* Single substitution mode: copy rest verbatim. */
      int rest = slen - si;
      if (oi + rest > out_max) return -1;
      uc_memcpy(out + oi, src + si, rest);
      oi += rest;
      si = slen;
      break;
    }
  }
  /* Copy any tail. */
  int tail = slen - si;
  if (oi + tail > out_max) return -1;
  uc_memcpy(out + oi, src + si, tail);
  oi += tail;
  *out_len = oi;
  return subs;
}

/* ── Input layer ─────────────────────────────────────────────────────── */

/* Read one line from fd into `out` (up to max bytes, no trailing \n).
 * Returns line length, or -1 at EOF.  Sets *eof_after if this read
 * exhausted the stream. */
typedef struct {
  int fd;
  char buf[256];
  int buf_pos;
  int buf_end;
  int saw_eof;
} input_t;

static int input_getline(input_t *in, char *out, int max, int *eof_after) {
  int len = 0;
  *eof_after = 0;
  for (;;) {
    if (in->buf_pos >= in->buf_end) {
      if (in->saw_eof) {
        if (len > 0) {
          *eof_after = 1;
          return len;
        }
        return -1;
      }
      ssize_t n = read(in->fd, in->buf, sizeof(in->buf));
      if (n <= 0) {
        in->saw_eof = 1;
        continue;
      }
      in->buf_pos = 0;
      in->buf_end = (int)n;
    }
    char c = in->buf[in->buf_pos++];
    if (c == '\n') return len;
    if (len < max - 1) out[len++] = c;
  }
}

/* Peek to detect last line: try to fetch one byte ahead.  Returns 1 if
 * EOF imminent (no more data after current line), else 0. */
static int input_at_eof(input_t *in) {
  if (in->buf_pos < in->buf_end) return 0;
  if (in->saw_eof) return 1;
  ssize_t n = read(in->fd, in->buf, sizeof(in->buf));
  if (n <= 0) {
    in->saw_eof = 1;
    return 1;
  }
  in->buf_pos = 0;
  in->buf_end = (int)n;
  return 0;
}

/* ── Main run loop ───────────────────────────────────────────────────── */

static char pspace[PATTERN_SPACE_SIZE];
static char tmp_space[PATTERN_SPACE_SIZE];

static void emit_line(const char *s, int n) {
  if (n > 0) write(1, s, (size_t)n);
  uc_putc('\n');
}

/* Returns 0 on normal completion, 1 if 'q' triggered quit. */
static int process_fd(int fd) {
  input_t in;
  in.fd = fd;
  in.buf_pos = 0;
  in.buf_end = 0;
  in.saw_eof = 0;
  int lineno = 0;
  int plen = 0;
  int eof_after = 0;
  int has_line = 0;
  for (;;) {
    if (!has_line) {
      plen = input_getline(&in, pspace, PATTERN_SPACE_SIZE, &eof_after);
      if (plen < 0) return 0;
      lineno++;
      last_line_seen = eof_after || input_at_eof(&in);
      has_line = 1;
    }
    int delete = 0;
    int quit = 0;
    for (int i = 0; i < n_cmds; i++) {
      command_t *c = &cmds[i];
      if (!cmd_addr_match(c, lineno, pspace, plen)) continue;
      switch (c->type) {
        case CMD_D:
          delete = 1;
          goto end_cmds;
        case CMD_P:
          emit_line(pspace, plen);
          break;
        case CMD_EQ:
          uc_printf("%u\n", (unsigned)lineno);
          break;
        case CMD_Q:
          quit = 1;
          goto end_cmds;
        case CMD_N_LOWER: {
          if (!n_flag) emit_line(pspace, plen);
          int el = 0;
          int nl = input_getline(&in, pspace, PATTERN_SPACE_SIZE, &el);
          if (nl < 0) {
            has_line = 0;
            return 0;
          }
          lineno++;
          plen = nl;
          last_line_seen = el || input_at_eof(&in);
          break;
        }
        case CMD_N_UPPER: {
          if (plen + 1 >= PATTERN_SPACE_SIZE) {
            uc_eputs("sed: pattern space overflow on N\n");
            return 1;
          }
          pspace[plen++] = '\n';
          int el = 0;
          int nl = input_getline(&in, pspace + plen,
                                 PATTERN_SPACE_SIZE - plen, &el);
          if (nl < 0) {
            return 0;
          }
          plen += nl;
          lineno++;
          last_line_seen = el || input_at_eof(&in);
          break;
        }
        case CMD_S: {
          int olen;
          int subs = do_subst(c, pspace, plen, tmp_space,
                              PATTERN_SPACE_SIZE, &olen);
          if (subs < 0) {
            uc_eputs("sed: pattern space overflow on s\n");
            return 1;
          }
          if (subs > 0) {
            uc_memcpy(pspace, tmp_space, olen);
            plen = olen;
            if (c->s_print) emit_line(pspace, plen);
          }
          break;
        }
      }
    }
  end_cmds:
    if (!delete && !n_flag) emit_line(pspace, plen);
    has_line = 0;
    if (quit) return 1;
  }
}

/* ── main ────────────────────────────────────────────────────────────── */

static int append_script_text(const char *text) {
  return parse_script(text);
}

static int append_script_file(const char *path) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    uc_eputs("sed: cannot open script: ");
    uc_eputs(path);
    uc_eputs("\n");
    return -1;
  }
  if (pool_used >= POOL_SIZE) {
    close(fd);
    return -1;
  }
  char *base = pool + pool_used;
  int max = POOL_SIZE - pool_used - 1;
  int total = 0;
  while (total < max) {
    ssize_t n = read(fd, base + total, (size_t)(max - total));
    if (n <= 0) break;
    total += (int)n;
  }
  base[total] = '\0';
  pool_used += total + 1;
  close(fd);
  return parse_script(base);
}

int main(int argc, char *argv[]) {
  int script_seen = 0;
  int argi = 1;
  while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
    if (uc_strcmp(argv[argi], "--") == 0) {
      argi++;
      break;
    }
    if (uc_strcmp(argv[argi], "--help") == 0) {
      uc_puts(
          "Usage: sed [-n] [-e SCRIPT]... [-f SCRIPTFILE]... [SCRIPT] "
          "[file ...]\n"
          "  -n  Suppress default print\n"
          "  -e  Append SCRIPT to the program\n"
          "  -f  Read SCRIPT from file\n"
          "Commands: s/RE/REPL/[gN p], d, p, =, q, n, N, # comment.\n"
          "BRE only: . * ^ $ [...] \\(...\\) \\1..\\9\n");
      return 0;
    }
    const char *p = argv[argi] + 1;
    if (*p == 'n' && p[1] == '\0') {
      n_flag = 1;
      argi++;
      continue;
    }
    if (*p == 'e') {
      const char *script;
      if (p[1] != '\0') {
        script = p + 1;
      } else if (argi + 1 < argc) {
        script = argv[++argi];
      } else {
        uc_eputs("sed: -e needs an argument\n");
        return 1;
      }
      if (append_script_text(script) < 0) {
        uc_eputs("sed: bad script\n");
        return 1;
      }
      script_seen = 1;
      argi++;
      continue;
    }
    if (*p == 'f') {
      const char *path;
      if (p[1] != '\0') {
        path = p + 1;
      } else if (argi + 1 < argc) {
        path = argv[++argi];
      } else {
        uc_eputs("sed: -f needs an argument\n");
        return 1;
      }
      if (append_script_file(path) < 0) return 1;
      script_seen = 1;
      argi++;
      continue;
    }
    uc_eputs("sed: unknown option: ");
    uc_eputs(argv[argi]);
    uc_eputs("\n");
    return 1;
  }
  if (!script_seen) {
    if (argi >= argc) {
      uc_eputs("sed: missing script\n");
      return 1;
    }
    if (append_script_text(argv[argi++]) < 0) {
      uc_eputs("sed: bad script\n");
      return 1;
    }
  }

  int rc = 0;
  if (argi >= argc) {
    rc = process_fd(0);
  } else {
    for (int i = argi; i < argc; i++) {
      int fd;
      if (uc_strcmp(argv[i], "-") == 0) {
        fd = 0;
      } else {
        fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
          uc_eputs("sed: ");
          uc_eputs(argv[i]);
          uc_eputs(": No such file or directory\n");
          return 1;
        }
      }
      int q = process_fd(fd);
      if (fd != 0) close(fd);
      if (q) {
        rc = 0;
        break;
      }
    }
  }
  return rc;
}
