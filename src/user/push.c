/*
 * push.c — PiPAPo μShell (Phase 1+2+3)
 *
 * Minimal shell for PiPAPo.  No libc dependency, static memory only.
 * Phase 1: quoting, $expansion, builtins, [[ ]], redirects, pipes,
 *          &&/||/; chaining, 4-tier PATH search, local/global env vars.
 * Phase 2: if/elif/else/fi, while/do/done, break/continue,
 *          $(...) command substitution, positional parameters.
 * Phase 3: interactive line editing, history, tab completion, PS1 prompt.
 */

#include "push.h"

#include "syscall.h"

/* ── Configuration ───────────────────────────────────────────────────── */

#define PUSH_LINE_MAX 256
#define TOK_BUF_SIZE 384 /* expanded token buffer (vars can grow)  */
#define TOKEN_MAX 64
#define ARGV_MAX 32
#define ENV_MAX 64
#define ENV_POOL_SIZE 1536
#define PIPE_MAX 4
#define PATH_BUF 128
#define REDIR_MAX 4
#define CAPTURE_BUF 256     /* $(...) output capture buffer          */
#define POS_PARAM_MAX 10    /* $0..$9                                */
#define SCRIPT_BUF_MAX 2048 /* max script size for compound stmts    */

#define W_OK 2
#define X_OK 1

/* ── Forward declarations ────────────────────────────────────────────── */

static int run_file(const char *path);
static int execute_line(const char *line);
struct line_src;
static int exec_from_source(struct line_src *ls);
static int exec_lines(const char **lines, int nlines);

/* ── Global state ────────────────────────────────────────────────────── */

static int last_status;
static int shell_pid;
static const char *shell_name;

/* Positional parameters ($1..$9, $0 = shell_name) */
static const char *pos_params[POS_PARAM_MAX]; /* $0..$9 */
static int pos_param_count;                   /* argc - 1 (num of $1..$9) */

/* Loop control */
static int break_pending;    /* >0: break N levels */
static int continue_pending; /* >0: continue N levels */

/* Environment pool: stores "KEY=VALUE\0" strings contiguously */
static char env_pool[ENV_POOL_SIZE];
static int env_pool_used;

static struct {
  unsigned short off;     /* offset into env_pool */
  unsigned char exported; /* 1 = passed to children via execve */
} env_tab[ENV_MAX];
static int env_count;

/* ── String helpers ──────────────────────────────────────────────────── */

static int my_strlen(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

static void puts_fd(int fd, const char *s) { write(fd, s, my_strlen(s)); }

static int streq(const char *a, const char *b) {
  while (*a && *b && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int strcaseeq(const char *a, const char *b) {
  while (*a && *b && to_lower(*a) == to_lower(*b)) {
    a++;
    b++;
  }
  return to_lower(*a) == to_lower(*b);
}

static void my_strcpy(char *dst, const char *src, int maxlen) {
  int i = 0;
  while (src[i] && i < maxlen - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static void my_strncpy(char *dst, const char *src, int n, int maxlen) {
  int i = 0;
  while (i < n && i < maxlen - 1 && src[i]) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static int my_atoi(const char *s) {
  int neg = 0, n = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  }
  while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
  return neg ? -n : n;
}

static void int_to_str(int val, char *buf, int size) {
  char tmp[12];
  int i = 0, neg = 0;
  if (val < 0) {
    neg = 1;
    val = -val;
  }
  if (val == 0) tmp[i++] = '0';
  while (val > 0 && i < 11) {
    tmp[i++] = '0' + val % 10;
    val /= 10;
  }
  int j = 0;
  if (neg && j < size - 1) buf[j++] = '-';
  while (i > 0 && j < size - 1) buf[j++] = tmp[--i];
  buf[j] = '\0';
}

static const char *my_strchr(const char *s, char c) {
  while (*s) {
    if (*s == c) return s;
    s++;
  }
  return 0;
}

static const char *my_strrchr(const char *s, char c) {
  const char *last = 0;
  while (*s) {
    if (*s == c) last = s;
    s++;
  }
  return last;
}

static void err_msg(const char *a, const char *b) {
  puts_fd(2, "push: ");
  puts_fd(2, a);
  if (b) {
    puts_fd(2, ": ");
    puts_fd(2, b);
  }
  puts_fd(2, "\n");
}

/* ── Environment management ──────────────────────────────────────────── */

static void env_copy(int dst, int src) {
  env_tab[dst].off = env_tab[src].off;
  env_tab[dst].exported = env_tab[src].exported;
}

static int env_find(const char *key) {
  int klen = my_strlen(key);
  for (int i = 0; i < env_count; i++) {
    const char *s = env_pool + env_tab[i].off;
    int j = 0;
    while (j < klen && s[j] == key[j]) j++;
    if (j == klen && s[j] == '=') return i;
  }
  return -1;
}

static const char *env_get(const char *key) {
  int idx = env_find(key);
  if (idx < 0) return 0;
  const char *s = env_pool + env_tab[idx].off;
  while (*s && *s != '=') s++;
  return *s ? s + 1 : "";
}

static void env_compact(void) {
  int pos = 0;
  for (int i = 0; i < env_count; i++) {
    const char *s = env_pool + env_tab[i].off;
    int len = my_strlen(s) + 1;
    if (env_tab[i].off != (unsigned short)pos) {
      for (int j = 0; j < len; j++) env_pool[pos + j] = s[j];
    }
    env_tab[i].off = pos;
    pos += len;
  }
  env_pool_used = pos;
}

/* Set env variable.  exported: 0=local, 1=global, -1=keep current */
static int env_set(const char *key, const char *value, int exported) {
  int idx = env_find(key);
  int klen = my_strlen(key);
  int vlen = my_strlen(value);
  int need = klen + 1 + vlen + 1; /* KEY=VALUE\0 */

  if (idx >= 0) {
    int old_exp = env_tab[idx].exported;
    for (int i = idx; i < env_count - 1; i++) env_copy(i, i + 1);
    env_count--;
    if (exported == -1) exported = old_exp;
  } else if (exported == -1) {
    exported = 0;
  }

  if (env_count >= ENV_MAX) return -1;

  if (env_pool_used + need > ENV_POOL_SIZE) {
    env_compact();
    if (env_pool_used + need > ENV_POOL_SIZE) return -1;
  }

  env_tab[env_count].off = env_pool_used;
  env_tab[env_count].exported = exported;

  char *dst = env_pool + env_pool_used;
  for (int i = 0; i < klen; i++) *dst++ = key[i];
  *dst++ = '=';
  for (int i = 0; i < vlen; i++) *dst++ = value[i];
  *dst++ = '\0';

  env_pool_used += need;
  env_count++;
  return 0;
}

static void env_unset(const char *key) {
  int idx = env_find(key);
  if (idx < 0) return;
  for (int i = idx; i < env_count - 1; i++) env_copy(i, i + 1);
  env_count--;
}

static int build_envp(char **envp, int max) {
  int n = 0;
  for (int i = 0; i < env_count && n < max - 1; i++) {
    if (env_tab[i].exported) envp[n++] = env_pool + env_tab[i].off;
  }
  envp[n] = 0;
  return n;
}

static void env_init(char **envp) {
  if (!envp) return;
  for (int i = 0; envp[i] && env_count < ENV_MAX; i++) {
    const char *s = envp[i];
    int len = my_strlen(s) + 1;
    if (env_pool_used + len > ENV_POOL_SIZE) break;
    env_tab[env_count].off = env_pool_used;
    env_tab[env_count].exported = 1;
    for (int j = 0; j < len; j++) env_pool[env_pool_used + j] = s[j];
    env_pool_used += len;
    env_count++;
  }
}

/* ── Forward declaration for command substitution ────────────────────── */

static void expand_cmd_subst(const char **pp, char **outp, char *end);

/* ── Variable expansion helper ───────────────────────────────────────── */

static void expand_var(const char **pp, char **outp, char *end) {
  const char *p = *pp + 1; /* skip '$' */

  if (*p == '?') {
    char tmp[12];
    int_to_str(last_status, tmp, sizeof(tmp));
    for (int i = 0; tmp[i] && *outp < end; i++) *(*outp)++ = tmp[i];
    p++;
  } else if (*p == '$') {
    char tmp[12];
    int_to_str(shell_pid, tmp, sizeof(tmp));
    for (int i = 0; tmp[i] && *outp < end; i++) *(*outp)++ = tmp[i];
    p++;
  } else if (*p == '(') {
    /* $(...) command substitution */
    const char *sub = *pp + 1; /* points at '(' */
    expand_cmd_subst(&sub, outp, end);
    *pp = sub;
    return;
  } else if (*p == '{') {
    p++;
    const char *start = p;
    while (*p && *p != '}') p++;
    char key[64];
    my_strncpy(key, start, (int)(p - start), sizeof(key));
    const char *val = env_get(key);
    if (val)
      for (int i = 0; val[i] && *outp < end; i++) *(*outp)++ = val[i];
    if (*p) p++;
  } else if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
             *p == '_') {
    const char *start = p;
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
           (*p >= '0' && *p <= '9') || *p == '_')
      p++;
    char key[64];
    my_strncpy(key, start, (int)(p - start), sizeof(key));
    const char *val = env_get(key);
    if (val)
      for (int i = 0; val[i] && *outp < end; i++) *(*outp)++ = val[i];
  } else if (*p >= '0' && *p <= '9') {
    int idx = *p - '0';
    const char *val = (idx == 0)                 ? shell_name
                      : (idx <= pos_param_count) ? pos_params[idx]
                                                 : 0;
    if (val)
      for (int i = 0; val[i] && *outp < end; i++) *(*outp)++ = val[i];
    p++;
  } else if (*p == '#') {
    char tmp[12];
    int_to_str(pos_param_count, tmp, sizeof(tmp));
    for (int i = 0; tmp[i] && *outp < end; i++) *(*outp)++ = tmp[i];
    p++;
  } else if (*p == '@') {
    for (int k = 1; k <= pos_param_count; k++) {
      if (k > 1 && *outp < end) *(*outp)++ = ' ';
      const char *val = pos_params[k];
      if (val)
        for (int i = 0; val[i] && *outp < end; i++) *(*outp)++ = val[i];
    }
    p++;
  } else {
    /* Unknown — emit literal '$' */
    if (*outp < end) *(*outp)++ = '$';
  }

  *pp = p;
}

/* ── Command substitution $(...) ──────────────────────────────────────── */

/*
 * Capture output of a command into out buffer.
 * The input 'p' points to the '(' after '$'.
 * Advances *pp past the closing ')'.
 * Single-level only — nested $() is not supported.
 */
static void expand_cmd_subst(const char **pp, char **outp, char *end) {
  const char *p = *pp + 1; /* skip '(' */

  /* Extract command string up to matching ')' */
  int depth = 1;
  const char *cmd_start = p;
  while (*p && depth > 0) {
    if (*p == '(')
      depth++;
    else if (*p == ')')
      depth--;
    if (depth > 0) p++;
  }
  /* p now points at closing ')' or NUL */
  int cmd_len = (int)(p - cmd_start);
  if (*p == ')') p++;
  *pp = p;

  if (cmd_len <= 0 || cmd_len >= PUSH_LINE_MAX) return;

  char cmd_buf[PUSH_LINE_MAX];
  my_strncpy(cmd_buf, cmd_start, cmd_len, sizeof(cmd_buf));

  /* pipe + vfork + exec the command, capture stdout */
  int pfd[2];
  if (pipe(pfd) < 0) return;

  pid_t pid = vfork();
  if (pid == 0) {
    /* child: redirect stdout to pipe write end */
    close(pfd[0]);
    dup2(pfd[1], 1);
    close(pfd[1]);
    /* Re-exec ourselves with -c to evaluate the command.
     * But we don't have -c support yet, so use an alternate approach:
     * write the command to a temp pipe and exec push reading from stdin.
     * Simpler: just call execute_line directly in the child.
     * vfork shares address space so we CAN call execute_line,
     * but it modifies globals. Since the child will _exit after,
     * and the parent is suspended, this is safe. */
    execute_line(cmd_buf);
    _exit(last_status);
  }

  close(pfd[1]);

  /* Parent: read child's stdout into output buffer */
  char cap[CAPTURE_BUF];
  int cap_len = 0;
  while (cap_len < CAPTURE_BUF - 1) {
    int n = read(pfd[0], cap + cap_len, CAPTURE_BUF - 1 - cap_len);
    if (n <= 0) break;
    cap_len += n;
  }
  close(pfd[0]);

  int wstatus;
  if (pid > 0) waitpid(pid, &wstatus, 0);

  /* Strip trailing newlines */
  while (cap_len > 0 && (cap[cap_len - 1] == '\n' || cap[cap_len - 1] == '\r'))
    cap_len--;

  /* Emit captured output */
  for (int i = 0; i < cap_len && *outp < end; i++) *(*outp)++ = cap[i];
}

/* ── Tokenizer ───────────────────────────────────────────────────────── */

static int is_op_start(char c) {
  return c == '|' || c == '&' || c == ';' || c == '>' || c == '<';
}

/*
 * Tokenize input line into buf, producing token pointers in toks[].
 * Operator tokens point to string literals (not into buf).
 * Word tokens point into buf (NUL-terminated, with $VAR expanded).
 */
static int tokenize(const char *input, char *buf, int buf_size, char **toks,
                    int max_toks) {
  const char *p = input;
  char *out = buf;
  char *end = buf + buf_size - 1;
  int n = 0;

#define EMIT(c)                  \
  do {                           \
    if (out < end) *out++ = (c); \
  } while (0)

  while (*p && n < max_toks) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#') break;

    /* Multi-char operators (check longest first) */
    if (p[0] == '2' && p[1] == '>' && p[2] == '>') {
      toks[n++] = "2>>";
      p += 3;
      continue;
    }
    if (p[0] == '2' && p[1] == '>' && p[2] == '&' && p[3] == '1') {
      toks[n++] = "2>&1";
      p += 4;
      continue;
    }
    if (p[0] == '1' && p[1] == '>' && p[2] == '&' && p[3] == '2') {
      toks[n++] = "1>&2";
      p += 4;
      continue;
    }
    if (p[0] == '1' && p[1] == '>' && p[2] == '>') {
      toks[n++] = ">>";
      p += 3;
      continue;
    }
    if (p[0] == '2' && p[1] == '>') {
      toks[n++] = "2>";
      p += 2;
      continue;
    }
    if (p[0] == '1' && p[1] == '>') {
      toks[n++] = ">";
      p += 2;
      continue;
    }
    if (p[0] == '>' && p[1] == '>') {
      toks[n++] = ">>";
      p += 2;
      continue;
    }
    if (p[0] == '&' && p[1] == '&') {
      toks[n++] = "&&";
      p += 2;
      continue;
    }
    if (p[0] == '|' && p[1] == '|') {
      toks[n++] = "||";
      p += 2;
      continue;
    }

    /* Single-char operators */
    if (*p == '>') {
      toks[n++] = ">";
      p++;
      continue;
    }
    if (*p == '<') {
      toks[n++] = "<";
      p++;
      continue;
    }
    if (*p == '|') {
      toks[n++] = "|";
      p++;
      continue;
    }
    if (*p == ';') {
      toks[n++] = ";";
      p++;
      continue;
    }

    /* Word token */
    toks[n] = out;

    while (*p && *p != ' ' && *p != '\t' && *p != '#') {
      if (is_op_start(*p)) break;
      /* Also break on ')' if standalone — but not inside word */

      if (*p == '\\' && p[1]) {
        p++;
        EMIT(*p++);
      } else if (*p == '\'') {
        p++;
        while (*p && *p != '\'') EMIT(*p++);
        if (*p) p++;
      } else if (*p == '"') {
        p++;
        while (*p && *p != '"') {
          if (*p == '\\' && p[1]) {
            p++;
            EMIT(*p++);
          } else if (*p == '$') {
            expand_var(&p, &out, end);
          } else {
            EMIT(*p++);
          }
        }
        if (*p) p++;
      } else if (*p == '$') {
        expand_var(&p, &out, end);
      } else {
        EMIT(*p++);
      }
    }

    *out++ = '\0';
    n++;
  }

#undef EMIT
  return n;
}

/* ── PATH search (4-tier priority) ───────────────────────────────────── */

/*
 * Match score for PATH search:
 *   1 = exact filename match
 *   2 = case-insensitive exact match
 *   3 = basename match (extension stripped)
 *   4 = case-insensitive basename match
 *   5 = no match
 */
/*
 * Check if extension ext (without dot) is listed in PATHEXT.
 * PATHEXT uses Windows format: ".COM;.OBJ;.X;.R" (semicolon-separated,
 * dot-prefixed, case-insensitive).  Returns 1 if ext is allowed.
 */
int ext_allowed(const char *ext, const char *pathext) {
  if (!pathext) return 1; /* unset → allow all */
  while (*pathext) {
    if (*pathext == '.') pathext++; /* skip leading dot */
    const char *sep = pathext;
    while (*sep && *sep != ';') sep++;
    int elen = (int)(sep - pathext);
    int xlen = my_strlen(ext);
    if (elen == xlen) {
      int match = 1;
      for (int i = 0; i < elen; i++) {
        char a = pathext[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) {
          match = 0;
          break;
        }
      }
      if (match) return 1;
    }
    pathext = *sep ? sep + 1 : sep;
  }
  return 0;
}

static int match_score(const char *want, const char *have) {
  if (streq(want, have)) return 1;
  if (strcaseeq(want, have)) return 2;
  const char *dot = my_strrchr(have, '.');
  if (dot) {
    const char *pathext = env_get("PATHEXT");
    if (!ext_allowed(dot + 1, pathext)) return 5;
    char base[PPAP_NAME_MAX + 1];
    my_strncpy(base, have, (int)(dot - have), sizeof(base));
    if (streq(want, base)) return 3;
    if (strcaseeq(want, base)) return 4;
  }
  return 5;
}

static void build_path(char *dst, int size, const char *dir, const char *name) {
  int i = 0;
  for (int j = 0; dir[j] && i < size - 2; j++) dst[i++] = dir[j];
  if (i > 0 && dst[i - 1] != '/') dst[i++] = '/';
  for (int j = 0; name[j] && i < size - 1; j++) dst[i++] = name[j];
  dst[i] = '\0';
}

#define SKIP_MAX 4

static int search_path_skip(const char *name, char *result, int rsize,
                            char skip[][PATH_BUF], int nskip) {
  if (my_strchr(name, '/')) {
    my_strcpy(result, name, rsize);
    return 0;
  }

  const char *path = env_get("PATH");
  if (!path) path = "/bin:/sbin";

  int best_score = 5;
  char best_path[PATH_BUF];

  while (*path) {
    const char *sep = path;
    while (*sep && *sep != ':') sep++;

    char dir[PATH_BUF];
    int dlen = (int)(sep - path);
    if (dlen == 0) {
      dir[0] = '.';
      dir[1] = '\0';
    } else {
      my_strncpy(dir, path, dlen, sizeof(dir));
    }

    int dfd = open(dir, O_RDONLY, 0);
    if (dfd >= 0) {
      struct dirent de;
      while (getdents(dfd, &de, sizeof(de)) > 0) {
        if (de.d_type == DT_DIR) continue;
        int score = match_score(name, de.d_name);
        if (score < best_score) {
          char cand[PATH_BUF];
          build_path(cand, sizeof(cand), dir, de.d_name);
          /* Check skip list */
          int skipped = 0;
          for (int j = 0; j < nskip; j++) {
            if (streq(cand, skip[j])) {
              skipped = 1;
              break;
            }
          }
          if (skipped) continue;
          best_score = score;
          my_strcpy(best_path, cand, sizeof(best_path));
          if (score == 1) {
            close(dfd);
            goto found;
          }
        }
      }
      close(dfd);
    }

    path = *sep ? sep + 1 : sep;
  }

  if (best_score >= 5) return -1;

found:
  my_strcpy(result, best_path, rsize);
  return 0;
}

static int search_path(const char *name, char *result, int rsize) {
  return search_path_skip(name, result, rsize, NULL, 0);
}

/* ── [[ ]] builtin test evaluator ────────────────────────────────────── */

static char **tt;
static int tt_n;
static int tt_pos;

static int t_expr(void);

static int t_primary(void) {
  if (tt_pos >= tt_n) return 0;
  const char *tok = tt[tt_pos];

  /* ( expr ) */
  if (streq(tok, "(")) {
    tt_pos++;
    int r = t_expr();
    if (tt_pos < tt_n && streq(tt[tt_pos], ")")) tt_pos++;
    return r;
  }

  /* Unary: -e, -f, -d, -r, -w, -x, -s, -z, -n */
  if (tok[0] == '-' && tok[1] && !tok[2] && tt_pos + 1 < tt_n) {
    const char *arg = tt[++tt_pos];
    tt_pos++;
    struct stat st;
    switch (tok[1]) {
      case 'e':
        return stat(arg, &st) == 0;
      case 'f':
        return stat(arg, &st) == 0 && S_ISREG(st.st_mode);
      case 'd':
        return stat(arg, &st) == 0 && S_ISDIR(st.st_mode);
      case 'r':
        return access(arg, R_OK) == 0;
      case 'w':
        return access(arg, W_OK) == 0;
      case 'x':
        return access(arg, X_OK) == 0;
      case 's':
        return stat(arg, &st) == 0 && st.st_size > 0;
      case 'z':
        return my_strlen(arg) == 0;
      case 'n':
        return my_strlen(arg) > 0;
    }
    return 0;
  }

  /* Binary: left OP right */
  if (tt_pos + 2 < tt_n) {
    const char *op = tt[tt_pos + 1];
    if (streq(op, "=") || streq(op, "!=") || streq(op, "-eq") ||
        streq(op, "-ne") || streq(op, "-lt") || streq(op, "-gt") ||
        streq(op, "-le") || streq(op, "-ge")) {
      const char *left = tt[tt_pos];
      const char *right = tt[tt_pos + 2];
      tt_pos += 3;
      if (streq(op, "=")) return streq(left, right);
      if (streq(op, "!=")) return !streq(left, right);
      int l = my_atoi(left), r = my_atoi(right);
      if (streq(op, "-eq")) return l == r;
      if (streq(op, "-ne")) return l != r;
      if (streq(op, "-lt")) return l < r;
      if (streq(op, "-gt")) return l > r;
      if (streq(op, "-le")) return l <= r;
      return l >= r; /* -ge */
    }
  }

  /* Bare word: true if non-empty */
  tt_pos++;
  return tok[0] != '\0';
}

static int t_not(void) {
  if (tt_pos < tt_n && streq(tt[tt_pos], "!")) {
    tt_pos++;
    return !t_not();
  }
  return t_primary();
}

static int t_and(void) {
  int r = t_not();
  while (tt_pos < tt_n && streq(tt[tt_pos], "&&")) {
    tt_pos++;
    int rr = t_not();
    r = r && rr;
  }
  return r;
}

static int t_expr(void) {
  int r = t_and();
  while (tt_pos < tt_n && streq(tt[tt_pos], "||")) {
    tt_pos++;
    int rr = t_and();
    r = r || rr;
  }
  return r;
}

/* Returns shell exit code: 0 = true, 1 = false */
static int eval_test(char **tokens, int count) {
  tt = tokens;
  tt_n = count;
  tt_pos = 0;
  return t_expr() ? 0 : 1;
}

/* ── Redirect parsing ────────────────────────────────────────────────── */

struct redir {
  const char *file; /* NULL for fd dup */
  int target_fd;    /* fd to redirect */
  int flags;        /* open flags */
  int source_fd;    /* >= 0 for fd dup, -1 for file */
};

static int parse_redirects(char **argv, int argc, struct redir *rr, int *nrr) {
  *nrr = 0;
  int out = 0;
  for (int i = 0; i < argc; i++) {
    if (*nrr >= REDIR_MAX) {
      argv[out++] = argv[i];
      continue;
    }

    if (streq(argv[i], ">") && i + 1 < argc) {
      rr[*nrr].file = argv[++i];
      rr[*nrr].target_fd = 1;
      rr[*nrr].flags = O_WRONLY | O_CREAT | O_TRUNC;
      rr[(*nrr)++].source_fd = -1;
    } else if (streq(argv[i], ">>") && i + 1 < argc) {
      rr[*nrr].file = argv[++i];
      rr[*nrr].target_fd = 1;
      rr[*nrr].flags = O_WRONLY | O_CREAT | O_APPEND;
      rr[(*nrr)++].source_fd = -1;
    } else if (streq(argv[i], "<") && i + 1 < argc) {
      rr[*nrr].file = argv[++i];
      rr[*nrr].target_fd = 0;
      rr[*nrr].flags = O_RDONLY;
      rr[(*nrr)++].source_fd = -1;
    } else if (streq(argv[i], "2>") && i + 1 < argc) {
      rr[*nrr].file = argv[++i];
      rr[*nrr].target_fd = 2;
      rr[*nrr].flags = O_WRONLY | O_CREAT | O_TRUNC;
      rr[(*nrr)++].source_fd = -1;
    } else if (streq(argv[i], "2>>") && i + 1 < argc) {
      rr[*nrr].file = argv[++i];
      rr[*nrr].target_fd = 2;
      rr[*nrr].flags = O_WRONLY | O_CREAT | O_APPEND;
      rr[(*nrr)++].source_fd = -1;
    } else if (streq(argv[i], "2>&1")) {
      rr[*nrr].file = 0;
      rr[*nrr].target_fd = 2;
      rr[*nrr].flags = 0;
      rr[(*nrr)++].source_fd = 1;
    } else if (streq(argv[i], "1>&2")) {
      rr[*nrr].file = 0;
      rr[*nrr].target_fd = 1;
      rr[*nrr].flags = 0;
      rr[(*nrr)++].source_fd = 2;
    } else {
      argv[out++] = argv[i];
    }
  }
  argv[out] = 0;
  return out;
}

static void apply_redirects(struct redir *rr, int nrr) {
  for (int i = 0; i < nrr; i++) {
    if (rr[i].source_fd >= 0) {
      dup2(rr[i].source_fd, rr[i].target_fd);
    } else {
      int fd = open(rr[i].file, rr[i].flags, 0644);
      if (fd >= 0) {
        dup2(fd, rr[i].target_fd);
        if (fd > 2) close(fd);
      }
    }
  }
}

/* Save/apply/restore redirects for builtins */
static void save_fds(struct redir *rr, int nrr, int saved[3]) {
  saved[0] = saved[1] = saved[2] = -1;
  for (int i = 0; i < nrr; i++) {
    int tfd = rr[i].target_fd;
    if (tfd < 3 && saved[tfd] < 0) saved[tfd] = dup(tfd);
  }
  apply_redirects(rr, nrr);
}

static void restore_fds(int saved[3]) {
  for (int i = 0; i < 3; i++) {
    if (saved[i] >= 0) {
      dup2(saved[i], i);
      close(saved[i]);
    }
  }
}

/* ── Builtins ────────────────────────────────────────────────────────── */

static int is_builtin(const char *cmd) {
  return streq(cmd, "exit") || streq(cmd, "true") || streq(cmd, "false") ||
         streq(cmd, "cd") || streq(cmd, "pwd") || streq(cmd, "echo") ||
         streq(cmd, "export") || streq(cmd, "unset") || streq(cmd, "set") ||
         streq(cmd, "env") || streq(cmd, ".") || streq(cmd, "source") ||
         streq(cmd, "break") || streq(cmd, "continue") || streq(cmd, "shift") ||
         streq(cmd, "history");
}

/* ── Public wrappers for push_line.c ──────────────────────────────────── */

const char *push_env_get(const char *key) { return env_get(key); }
int push_is_builtin(const char *cmd) { return is_builtin(cmd); }

/* Execute builtin.  Always returns 1 (handled).  Sets *status. */
static void run_builtin(char **argv, int argc, int *status) {
  const char *cmd = argv[0];

  if (streq(cmd, "exit")) _exit(argc > 1 ? my_atoi(argv[1]) : last_status);

  if (streq(cmd, "true")) {
    *status = 0;
    return;
  }
  if (streq(cmd, "false")) {
    *status = 1;
    return;
  }

  if (streq(cmd, "cd")) {
    const char *dir = argc > 1 ? argv[1] : env_get("HOME");
    if (!dir) dir = "/";
    if (streq(dir, "-")) {
      dir = env_get("OLDPWD");
      if (!dir) {
        err_msg("cd", "OLDPWD not set");
        *status = 1;
        return;
      }
    }
    char old[PATH_BUF];
    if (getcwd(old, sizeof(old)) > 0) env_set("OLDPWD", old, -1);
    if (chdir(dir) < 0) {
      err_msg("cd", dir);
      *status = 1;
    } else {
      char cwd[PATH_BUF];
      if (getcwd(cwd, sizeof(cwd)) > 0) env_set("PWD", cwd, -1);
      *status = 0;
    }
    return;
  }

  if (streq(cmd, "pwd")) {
    char cwd[PATH_BUF];
    if (getcwd(cwd, sizeof(cwd)) > 0) {
      puts_fd(1, cwd);
      write(1, "\n", 1);
      *status = 0;
    } else {
      *status = 1;
    }
    return;
  }

  if (streq(cmd, "echo")) {
    int no_nl = 0, first = 1;
    for (int i = 1; i < argc; i++) {
      if (i == 1 && streq(argv[i], "-n")) {
        no_nl = 1;
        continue;
      }
      if (!first) write(1, " ", 1);
      puts_fd(1, argv[i]);
      first = 0;
    }
    if (!no_nl) write(1, "\n", 1);
    *status = 0;
    return;
  }

  if (streq(cmd, "export")) {
    if (argc < 2) {
      for (int i = 0; i < env_count; i++) {
        if (env_tab[i].exported) {
          puts_fd(1, "export ");
          puts_fd(1, env_pool + env_tab[i].off);
          write(1, "\n", 1);
        }
      }
      *status = 0;
      return;
    }
    for (int i = 1; i < argc; i++) {
      const char *eq = my_strchr(argv[i], '=');
      if (eq) {
        char key[64];
        my_strncpy(key, argv[i], (int)(eq - argv[i]), sizeof(key));
        env_set(key, eq + 1, 1);
      } else {
        int idx = env_find(argv[i]);
        if (idx >= 0)
          env_tab[idx].exported = 1;
        else
          env_set(argv[i], "", 1);
      }
    }
    *status = 0;
    return;
  }

  if (streq(cmd, "unset")) {
    for (int i = 1; i < argc; i++) env_unset(argv[i]);
    *status = 0;
    return;
  }

  if (streq(cmd, "set")) {
    for (int i = 0; i < env_count; i++) {
      puts_fd(1, env_pool + env_tab[i].off);
      write(1, "\n", 1);
    }
    *status = 0;
    return;
  }

  if (streq(cmd, "env")) {
    for (int i = 0; i < env_count; i++) {
      if (env_tab[i].exported) {
        puts_fd(1, env_pool + env_tab[i].off);
        write(1, "\n", 1);
      }
    }
    *status = 0;
    return;
  }

  if (streq(cmd, ".") || streq(cmd, "source")) {
    if (argc < 2) {
      err_msg(cmd, "filename required");
      *status = 1;
      return;
    }
    *status = run_file(argv[1]);
    return;
  }

  if (streq(cmd, "break")) {
    break_pending = argc > 1 ? my_atoi(argv[1]) : 1;
    if (break_pending < 1) break_pending = 1;
    *status = 0;
    return;
  }

  if (streq(cmd, "continue")) {
    continue_pending = argc > 1 ? my_atoi(argv[1]) : 1;
    if (continue_pending < 1) continue_pending = 1;
    *status = 0;
    return;
  }

  if (streq(cmd, "shift")) {
    int n = argc > 1 ? my_atoi(argv[1]) : 1;
    if (n < 1) n = 1;
    if (n > pos_param_count) n = pos_param_count;
    for (int i = 1; i + n < POS_PARAM_MAX && i <= pos_param_count - n; i++)
      pos_params[i] = pos_params[i + n];
    pos_param_count -= n;
    *status = 0;
    return;
  }

  if (streq(cmd, "history")) {
    push_history_list(1);
    *status = 0;
    return;
  }

  *status = 127;
}

/* ── Simple command execution ────────────────────────────────────────── */

static int is_valid_ident_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static int exec_simple(char **argv, int argc) {
  if (argc == 0) return 0;

  /* Handle bare VAR=value assignment (no command follows) */
  {
    int i = 0;
    while (i < argc) {
      const char *eq = my_strchr(argv[i], '=');
      if (!eq || eq == argv[i]) break;
      /* Check chars before '=' are valid identifier */
      int valid = 1;
      for (const char *p = argv[i]; p < eq; p++) {
        if (!is_valid_ident_char(*p)) {
          valid = 0;
          break;
        }
      }
      if (!valid) break;
      i++;
    }
    if (i == argc) {
      /* All tokens are assignments */
      for (int j = 0; j < argc; j++) {
        const char *eq = my_strchr(argv[j], '=');
        char key[64];
        my_strncpy(key, argv[j], (int)(eq - argv[j]), sizeof(key));
        env_set(key, eq + 1, -1);
      }
      return 0;
    }
  }

  /* Parse redirections */
  struct redir redirs[REDIR_MAX];
  int nredirs;
  argc = parse_redirects(argv, argc, redirs, &nredirs);
  if (argc == 0) return 0;

  /* [[ ... ]] test */
  if (streq(argv[0], "[[")) {
    int end = -1;
    for (int i = 1; i < argc; i++) {
      if (streq(argv[i], "]]")) {
        end = i;
        break;
      }
    }
    if (end < 0) {
      err_msg("[[", "missing ]]");
      return 2;
    }
    /* Apply redirects (unusual but possible) */
    int saved[3];
    if (nredirs > 0) save_fds(redirs, nredirs, saved);
    int result = eval_test(argv + 1, end - 1);
    if (nredirs > 0) restore_fds(saved);
    return result;
  }

  /* exec builtin — replace shell */
  if (streq(argv[0], "exec")) {
    if (argc < 2) {
      /* exec with redirects only — apply to shell permanently */
      apply_redirects(redirs, nredirs);
      return 0;
    }
    apply_redirects(redirs, nredirs);
    char resolved[PATH_BUF];
    if (search_path(argv[1], resolved, sizeof(resolved)) < 0) {
      err_msg(argv[1], "not found");
      return 127;
    }
    char *envp[ENV_MAX + 1];
    build_envp(envp, ENV_MAX + 1);
    execve(resolved, argv + 1, envp);
    err_msg(argv[1], "exec failed");
    return 126;
  }

  /* Other builtins */
  if (is_builtin(argv[0])) {
    int saved[3];
    if (nredirs > 0) save_fds(redirs, nredirs, saved);
    int status;
    run_builtin(argv, argc, &status);
    if (nredirs > 0) restore_fds(saved);
    return status;
  }

  /* External command — resolve via PATH, retry on ENOEXEC */
  char resolved[PATH_BUF];
  static char tried[SKIP_MAX][PATH_BUF];
  int ntried = 0;

  if (search_path(argv[0], resolved, sizeof(resolved)) < 0) {
    err_msg(argv[0], "not found");
    return 127;
  }

  char *envp[ENV_MAX + 1];
  build_envp(envp, ENV_MAX + 1);

  for (;;) {
    /* Set argv[0] to the resolved path so loaders (e.g. CP/M)
     * can derive the application directory from it. */
    char *saved_argv0 = argv[0];
    argv[0] = resolved;

    pid_t pid = vfork();
    if (pid == 0) {
      apply_redirects(redirs, nredirs);
      execve(resolved, argv, envp);
      _exit(127);
    }
    argv[0] = saved_argv0;
    if (pid < 0) {
      err_msg(argv[0], "fork failed");
      return 1;
    }

    int wstatus;
    waitpid(pid, &wstatus, 0);
    int st = WIFEXITED(wstatus) ? (int)WEXITSTATUS(wstatus)
                                : (128 + (int)WTERMSIG(wstatus));

    /* If child exited 127 (execve failed), try next PATH candidate */
    if (st == 127 && ntried < SKIP_MAX) {
      my_strcpy(tried[ntried], resolved, PATH_BUF);
      ntried++;
      if (search_path_skip(argv[0], resolved, sizeof(resolved), tried,
                           ntried) == 0)
        continue;
    }
    return st;
  }
}

/* ── Pipeline execution ──────────────────────────────────────────────── */

static int exec_pipeline(char **toks, int ntoks, int *pos) {
  struct {
    char *argv[ARGV_MAX + 1];
    int argc;
  } stages[PIPE_MAX];
  int nstages = 0;
  int depth = 0; /* [[ ]] nesting depth */

  stages[0].argc = 0;

  while (*pos < ntoks) {
    const char *t = toks[*pos];

    if (streq(t, "[["))
      depth++;
    else if (streq(t, "]]")) {
      if (depth > 0) depth--;
    }

    if (depth == 0) {
      /* Stop at chain operators */
      if (streq(t, "&&") || streq(t, "||") || streq(t, ";")) break;

      /* Pipe separator */
      if (streq(t, "|")) {
        (*pos)++;
        nstages++;
        if (nstages >= PIPE_MAX) {
          err_msg("pipe", "too many stages");
          return 1;
        }
        stages[nstages].argc = 0;
        continue;
      }
    }

    if (stages[nstages].argc < ARGV_MAX)
      stages[nstages].argv[stages[nstages].argc++] = toks[*pos];
    (*pos)++;
  }
  nstages++;
  int stage_count = nstages;

  for (int i = 0; i < stage_count; i++) stages[i].argv[stages[i].argc] = 0;

  /* Single command — no pipe overhead */
  if (stage_count == 1) return exec_simple(stages[0].argv, stages[0].argc);

  /* Multi-stage pipeline */
  char *envp[ENV_MAX + 1];
  build_envp(envp, ENV_MAX + 1);

  pid_t pids[PIPE_MAX];
  int prev_read = -1;

  for (int i = 0; i < stage_count; i++) {
    int pipefd[2] = {-1, -1};
    if (i < stage_count - 1) {
      if (pipe(pipefd) < 0) {
        err_msg("pipe", "failed");
        return 1;
      }
    }

    struct redir redirs[REDIR_MAX];
    int nredirs;
    stages[i].argc =
        parse_redirects(stages[i].argv, stages[i].argc, redirs, &nredirs);

    char resolved[PATH_BUF];
    resolved[0] = '\0';
    if (stages[i].argc > 0 &&
        search_path(stages[i].argv[0], resolved, sizeof(resolved)) != 0)
      resolved[0] = '\0';

    pid_t pid = vfork();
    if (pid == 0) {
      if (prev_read >= 0) {
        dup2(prev_read, 0);
        close(prev_read);
      }
      if (pipefd[1] >= 0) {
        dup2(pipefd[1], 1);
        close(pipefd[1]);
      }
      if (pipefd[0] >= 0) close(pipefd[0]);
      apply_redirects(redirs, nredirs);
      if (resolved[0] != '\0' && stages[i].argc > 0)
        execve(resolved, stages[i].argv, envp);
      _exit(127);
    }

    pids[i] = pid;
    if (prev_read >= 0) close(prev_read);
    if (pipefd[1] >= 0) close(pipefd[1]);
    prev_read = pipefd[0];
  }

  /* Wait for all children; report last stage's exit status */
  int status = 0;
  for (int i = 0; i < stage_count; i++) {
    int wstatus;
    waitpid(pids[i], &wstatus, 0);
    if (i == stage_count - 1)
      status = WIFEXITED(wstatus) ? (int)WEXITSTATUS(wstatus)
                                  : (128 + (int)WTERMSIG(wstatus));
  }
  return status;
}

/* ── List execution (&&, ||, ;) ──────────────────────────────────────── */

/* Skip one pipeline's worth of tokens */
static void skip_pipeline(char **toks, int ntoks, int *pos) {
  int depth = 0;
  while (*pos < ntoks) {
    if (streq(toks[*pos], "[["))
      depth++;
    else if (streq(toks[*pos], "]]")) {
      if (depth > 0) depth--;
    } else if (depth == 0) {
      if (streq(toks[*pos], ";") || streq(toks[*pos], "&&") ||
          streq(toks[*pos], "||"))
        return;
    }
    (*pos)++;
  }
}

static int exec_list(char **toks, int ntoks) {
  int pos = 0;
  int status = 0;

  while (pos < ntoks) {
    /* Skip empty segments */
    while (pos < ntoks && streq(toks[pos], ";")) pos++;
    if (pos >= ntoks) break;

    status = exec_pipeline(toks, ntoks, &pos);
    last_status = status;

    /* Handle chain operators */
    while (pos < ntoks) {
      if (streq(toks[pos], ";")) {
        pos++;
        break; /* next segment */
      }
      if (streq(toks[pos], "&&")) {
        pos++;
        if (status != 0) {
          skip_pipeline(toks, ntoks, &pos);
        } else {
          break; /* execute next pipeline */
        }
        continue;
      }
      if (streq(toks[pos], "||")) {
        pos++;
        if (status == 0) {
          skip_pipeline(toks, ntoks, &pos);
        } else {
          break; /* execute next pipeline */
        }
        continue;
      }
      break; /* not a chain op, done */
    }
  }
  return status;
}

/* ── Line execution ──────────────────────────────────────────────────── */

static int execute_line(const char *line) {
  char buf[TOK_BUF_SIZE];
  char *toks[TOKEN_MAX];
  int ntoks = tokenize(line, buf, sizeof(buf), toks, TOKEN_MAX);
  if (ntoks <= 0) return 0;

  int status = exec_list(toks, ntoks);
  last_status = status;
  return status;
}

/* ── File reader ─────────────────────────────────────────────────────── */

static int read_line(int fd, char *buf, int len) {
  int i = 0;
  while (i < len - 1) {
    char c;
    int n = read(fd, &c, 1);
    if (n <= 0) return i > 0 ? i : -1;
    if (c == '\n') break;
    if (c == '\r') continue;
    buf[i++] = c;
  }
  buf[i] = '\0';
  return i;
}

/* ── Script line array ───────────────────────────────────────────────── */

/*
 * For compound statements (if/while), we need to look ahead and collect
 * multiple lines.  A "line source" abstracts reading from a file or from
 * a pre-collected line array (for nested compounds).
 */
struct line_src {
  int fd;             /* file descriptor (-1 if using lines[]) */
  const char **lines; /* pre-collected line array (NULL if fd) */
  int nlines;         /* number of lines in array */
  int pos;            /* current position in lines[] */
  int first;          /* first-line flag (for shebang skip) */
};

static int ls_next(struct line_src *ls, char *buf, int len) {
  if (ls->lines) {
    if (ls->pos >= ls->nlines) return -1;
    my_strcpy(buf, ls->lines[ls->pos++], len);
    return my_strlen(buf);
  }
  return read_line(ls->fd, buf, len);
}

/* Check if a token matches a keyword (first word on a line) */
static int is_keyword(const char *line, const char *kw) {
  while (*line == ' ' || *line == '\t') line++;
  int len = my_strlen(kw);
  int i = 0;
  while (i < len && line[i] == kw[i]) i++;
  if (i != len) return 0;
  /* Must be followed by space, tab, NUL, or ';' */
  return line[i] == '\0' || line[i] == ' ' || line[i] == '\t' ||
         line[i] == ';' || line[i] == '#';
}

/* Check if line starts with keyword (ignoring leading whitespace) */
static const char *skip_ws(const char *s) {
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

/*
 * Collect lines for a compound statement body.
 * Reads lines from ls until a terminator keyword is found.
 * Returns collected lines in out_lines[] (pointers into line_pool).
 * The terminator line is NOT consumed — caller checks it.
 *
 * For if:  terminators are "elif", "else", "fi"
 * For while: terminator is "done"
 *
 * Handles nesting: inner if/while blocks are collected as-is.
 */
static char *compound_pool; /* lazily allocated via brk() */
static int compound_pool_used;

static int ensure_compound_pool(void) {
  if (compound_pool) return 0;
  void *cur = brk(0);
  if (!cur) return -1;
  void *nxt = brk((char *)cur + SCRIPT_BUF_MAX);
  if (nxt == cur) return -1; /* brk unchanged = failure */
  compound_pool = (char *)cur;
  return 0;
}

/*
 * collect_body: read lines from ls until a terminator keyword is found.
 * found_term receives the keyword name (e.g., "fi", "elif", "else", "done").
 * found_line receives the FULL terminator line (for elif condition extraction).
 * Returns number of body lines collected.
 */
static int collect_body(struct line_src *ls, const char **out_lines,
                        int max_lines, const char **term_kws, int n_term,
                        char *found_term, int found_size, char *found_line,
                        int found_line_size) {
  if (ensure_compound_pool() < 0) {
    err_msg("compound", "out of memory");
    found_term[0] = '\0';
    if (found_line) found_line[0] = '\0';
    return 0;
  }

  int n = 0;
  int nest_if = 0, nest_while = 0;
  char linebuf[PUSH_LINE_MAX];

  while (ls_next(ls, linebuf, sizeof(linebuf)) >= 0) {
    const char *trimmed = skip_ws(linebuf);

    /* Track nesting of inner if/while blocks */
    if (is_keyword(trimmed, "if")) nest_if++;
    if (is_keyword(trimmed, "while")) nest_while++;

    if (is_keyword(trimmed, "fi") && nest_if > 0) {
      nest_if--;
      goto store;
    }
    if (is_keyword(trimmed, "done") && nest_while > 0) {
      nest_while--;
      goto store;
    }

    /* Check for terminator at nesting level 0 */
    if (nest_if == 0 && nest_while == 0) {
      for (int i = 0; i < n_term; i++) {
        if (is_keyword(trimmed, term_kws[i])) {
          my_strcpy(found_term, term_kws[i], found_size);
          if (found_line) my_strcpy(found_line, linebuf, found_line_size);
          return n;
        }
      }
    }

  store:
    /* Store line in pool */
    {
      int len = my_strlen(linebuf) + 1;
      if (compound_pool_used + len > SCRIPT_BUF_MAX || n >= max_lines) {
        err_msg("compound", "too large");
        return n;
      }
      char *dst = compound_pool + compound_pool_used;
      my_strcpy(dst, linebuf, len);
      out_lines[n++] = dst;
      compound_pool_used += len;
    }
  }

  /* EOF without terminator */
  found_term[0] = '\0';
  if (found_line) found_line[0] = '\0';
  return n;
}

/*
 * Extract condition from an "if COND; then" or "elif COND; then" line.
 * skip_len is the keyword length to skip (2 for "if", 4 for "elif").
 * Strips trailing "; then" or "; do".
 */
static void extract_condition(const char *line, int skip_len, char *cond_buf,
                              int cond_size, const char *trail) {
  const char *p = skip_ws(line) + skip_len;
  while (*p == ' ' || *p == '\t') p++;
  my_strcpy(cond_buf, p, cond_size);

  int clen = my_strlen(cond_buf);
  int tlen = my_strlen(trail);
  if (clen >= tlen) {
    char *t = cond_buf + clen - tlen;
    if (streq(t, trail)) {
      *t = '\0';
      clen -= tlen;
      while (clen > 0 &&
             (cond_buf[clen - 1] == ' ' || cond_buf[clen - 1] == ';' ||
              cond_buf[clen - 1] == '\t'))
        cond_buf[--clen] = '\0';
    }
  }
}

/*
 * Execute an if/elif/else/fi block.
 * 'if_line' is the full "if COND; then" line.
 * Reads subsequent lines from ls for body, elif, else, fi.
 */
static int exec_if(const char *if_line, struct line_src *ls) {
  int save_pool = compound_pool_used;
  int done = 0;
  int status = 0;

  /* Evaluate initial "if" condition */
  char cond_buf[PUSH_LINE_MAX];
  extract_condition(if_line, 2, cond_buf, sizeof(cond_buf), "then");
  status = execute_line(cond_buf);
  last_status = status;

  const char *if_terms[] = {"elif", "else", "fi"};
  const char *body_lines[128];
  char term[16], term_line[PUSH_LINE_MAX];

  int nbody = collect_body(ls, body_lines, 128, if_terms, 3, term, sizeof(term),
                           term_line, sizeof(term_line));

  if (status == 0 && !done) {
    exec_lines(body_lines, nbody);
    done = 1;
  }

  /* Handle elif chain */
  while (streq(term, "elif")) {
    compound_pool_used = save_pool;

    /* Extract elif condition from the saved terminator line */
    extract_condition(term_line, 4, cond_buf, sizeof(cond_buf), "then");
    if (!done) {
      status = execute_line(cond_buf);
      last_status = status;
    }

    nbody = collect_body(ls, body_lines, 128, if_terms, 3, term, sizeof(term),
                         term_line, sizeof(term_line));

    if (!done && status == 0) {
      exec_lines(body_lines, nbody);
      done = 1;
    }
  }

  /* Handle else */
  if (streq(term, "else")) {
    compound_pool_used = save_pool;
    const char *fi_terms[] = {"fi"};
    nbody = collect_body(ls, body_lines, 128, fi_terms, 1, term, sizeof(term),
                         term_line, sizeof(term_line));
    if (!done) {
      exec_lines(body_lines, nbody);
    }
  }

  compound_pool_used = save_pool;
  return last_status;
}

/*
 * Execute a while/do/done block.
 * 'while_line' is the full "while COND; do" line.
 */
static int exec_while(const char *while_line, struct line_src *ls) {
  int save_pool = compound_pool_used;
  int status = 0;

  char cond_buf[PUSH_LINE_MAX];
  extract_condition(while_line, 5, cond_buf, sizeof(cond_buf), "do");

  const char *done_terms[] = {"done"};
  const char *body_lines[128];
  char term[16], term_line[PUSH_LINE_MAX];

  int nbody = collect_body(ls, body_lines, 128, done_terms, 1, term,
                           sizeof(term), term_line, sizeof(term_line));

  while (1) {
    status = execute_line(cond_buf);
    last_status = status;
    if (status != 0) break;

    exec_lines(body_lines, nbody);

    if (break_pending > 0) {
      break_pending--;
      break;
    }
    if (continue_pending > 0) {
      continue_pending--;
    }
  }

  compound_pool_used = save_pool;
  return last_status;
}

/* Execute a block of collected lines (for if-body, while-body, etc.) */
static int exec_lines(const char **lines, int nlines) {
  struct line_src ls;
  ls.fd = -1;
  ls.lines = lines;
  ls.nlines = nlines;
  ls.pos = 0;
  ls.first = 0;

  return exec_from_source(&ls);
}

/* ── Compound statement execution from line source ───────────────────── */

static int exec_from_source(struct line_src *ls) {
  int status = 0;
  char line[PUSH_LINE_MAX];

  while (ls_next(ls, line, sizeof(line)) >= 0) {
    if (ls->first && line[0] == '#' && line[1] == '!') {
      ls->first = 0;
      continue;
    }
    ls->first = 0;
    if (line[0] == '\0') continue;

    const char *trimmed = skip_ws(line);
    if (trimmed[0] == '#') continue;

    if (is_keyword(trimmed, "if")) {
      status = exec_if(line, ls);
      continue;
    }

    if (is_keyword(trimmed, "while")) {
      status = exec_while(line, ls);
      continue;
    }

    /* Regular line */
    status = execute_line(line);

    if (break_pending > 0 || continue_pending > 0) return status;
  }
  return status;
}

static int run_file(const char *path) {
  int fd, close_fd = 0;

  if (!path) {
    fd = 0;
  } else {
    fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
      err_msg(path, "cannot open");
      return 1;
    }
    close_fd = 1;
  }

  struct line_src ls;
  ls.fd = fd;
  ls.lines = 0;
  ls.nlines = 0;
  ls.pos = 0;
  ls.first = 1;

  int status = exec_from_source(&ls);

  if (close_fd) close(fd);
  return status;
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  shell_pid = getpid();
  shell_name = argc > 0 ? argv[0] : "push";

  /* Import environment from parent (envp follows argv on stack) */
  if (argc > 0 && argv[argc] == 0) {
    char **envp = &argv[argc + 1];
    env_init(envp);
  }

  /* Initialize PWD */
  char cwd[PATH_BUF];
  if (getcwd(cwd, sizeof(cwd)) > 0) env_set("PWD", cwd, 1);

  /* Script mode: push script.sh [args...] */
  if (argc > 1) {
    shell_name = argv[1];
    pos_params[0] = argv[1];
    pos_param_count = 0;
    for (int i = 2; i < argc && pos_param_count < POS_PARAM_MAX - 1; i++)
      pos_params[++pos_param_count] = argv[i];
    return run_file(argv[1]);
  }

  /* Interactive mode with line editing (Phase 3) */
  {
    /* Source /etc/profile if it exists */
    struct stat st;
    if (stat("/etc/profile", &st) == 0) run_file("/etc/profile");

    struct line_src ls;
    ls.fd = 0;
    ls.lines = 0;
    ls.nlines = 0;
    ls.pos = 0;
    ls.first = 0;

    /* Detect TERM — use raw readline only if TERM != "dumb" */
    const char *term = env_get("TERM");
    int use_readline = term && !streq(term, "dumb");

    char line[PUSH_LINE_MAX];
    for (;;) {
      int n;
      if (use_readline) {
        /* PS1-based prompt via push_readline */
        const char *ps1 = env_get("PS1");
        n = push_readline(ps1, line, sizeof(line));
      } else {
        /* Dumb terminal fallback */
        puts_fd(2, "$ ");
        n = read_line(0, line, sizeof(line));
      }
      if (n < 0) break;
      if (line[0] == '\0') continue;

      const char *trimmed = skip_ws(line);
      if (trimmed[0] == '#') continue;

      /* Handle compound statements in interactive mode */
      if (is_keyword(trimmed, "if")) {
        exec_if(line, &ls);
        continue;
      }
      if (is_keyword(trimmed, "while")) {
        exec_while(line, &ls);
        continue;
      }

      execute_line(line);
    }
  }
  write(2, "\n", 1);
  return last_status;
}
