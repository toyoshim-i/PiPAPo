/*
 * push.c — PiPAPo μShell (Phase 1: Core Interpreter)
 *
 * Minimal shell for PiPAPo.  No libc dependency, static memory only.
 * Features: quoting, $expansion, builtins, [[ ]], redirects, pipes,
 * &&/||/; chaining, 4-tier PATH search, local/global env vars.
 *
 * Phase 1 deliverable: script execution via #!/bin/push
 */

#include "syscall.h"

/* ── Configuration ───────────────────────────────────────────────────── */

#define PUSH_LINE_MAX    256
#define TOK_BUF_SIZE     384   /* expanded token buffer (vars can grow)  */
#define TOKEN_MAX        64
#define ARGV_MAX         32
#define ENV_MAX          64
#define ENV_POOL_SIZE    2048
#define PIPE_MAX         4
#define PATH_BUF         128
#define REDIR_MAX        4

#define W_OK 2
#define X_OK 1

/* ── Forward declarations ────────────────────────────────────────────── */

static int run_file(const char *path);

/* ── Global state ────────────────────────────────────────────────────── */

static int last_status;
static int shell_pid;
static const char *shell_name;

/* Environment pool: stores "KEY=VALUE\0" strings contiguously */
static char env_pool[ENV_POOL_SIZE];
static int  env_pool_used;

static struct {
    unsigned short off;       /* offset into env_pool */
    unsigned char  exported;  /* 1 = passed to children via execve */
} env_tab[ENV_MAX];
static int env_count;

/* ── String helpers ──────────────────────────────────────────────────── */

static int my_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void puts_fd(int fd, const char *s)
{
    write(fd, s, my_strlen(s));
}

static int streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static char to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int strcaseeq(const char *a, const char *b)
{
    while (*a && *b && to_lower(*a) == to_lower(*b)) { a++; b++; }
    return to_lower(*a) == to_lower(*b);
}

static void my_strcpy(char *dst, const char *src, int maxlen)
{
    int i = 0;
    while (src[i] && i < maxlen - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void my_strncpy(char *dst, const char *src, int n, int maxlen)
{
    int i = 0;
    while (i < n && i < maxlen - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int my_atoi(const char *s)
{
    int neg = 0, n = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}

static void int_to_str(int val, char *buf, int size)
{
    char tmp[12];
    int i = 0, neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    if (val == 0) tmp[i++] = '0';
    while (val > 0 && i < 11) { tmp[i++] = '0' + val % 10; val /= 10; }
    int j = 0;
    if (neg && j < size - 1) buf[j++] = '-';
    while (i > 0 && j < size - 1) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static const char *my_strchr(const char *s, char c)
{
    while (*s) { if (*s == c) return s; s++; }
    return 0;
}

static const char *my_strrchr(const char *s, char c)
{
    const char *last = 0;
    while (*s) { if (*s == c) last = s; s++; }
    return last;
}

static void err_msg(const char *a, const char *b)
{
    puts_fd(2, "push: ");
    puts_fd(2, a);
    if (b) { puts_fd(2, ": "); puts_fd(2, b); }
    puts_fd(2, "\n");
}

/* ── Environment management ──────────────────────────────────────────── */

static void env_copy(int dst, int src)
{
    env_tab[dst].off      = env_tab[src].off;
    env_tab[dst].exported = env_tab[src].exported;
}

static int env_find(const char *key)
{
    int klen = my_strlen(key);
    for (int i = 0; i < env_count; i++) {
        const char *s = env_pool + env_tab[i].off;
        int j = 0;
        while (j < klen && s[j] == key[j]) j++;
        if (j == klen && s[j] == '=')
            return i;
    }
    return -1;
}

static const char *env_get(const char *key)
{
    int idx = env_find(key);
    if (idx < 0) return 0;
    const char *s = env_pool + env_tab[idx].off;
    while (*s && *s != '=') s++;
    return *s ? s + 1 : "";
}

static void env_compact(void)
{
    int pos = 0;
    for (int i = 0; i < env_count; i++) {
        const char *s = env_pool + env_tab[i].off;
        int len = my_strlen(s) + 1;
        if (env_tab[i].off != pos) {
            for (int j = 0; j < len; j++)
                env_pool[pos + j] = s[j];
        }
        env_tab[i].off = pos;
        pos += len;
    }
    env_pool_used = pos;
}

/* Set env variable.  exported: 0=local, 1=global, -1=keep current */
static int env_set(const char *key, const char *value, int exported)
{
    int idx = env_find(key);
    int klen = my_strlen(key);
    int vlen = my_strlen(value);
    int need = klen + 1 + vlen + 1;  /* KEY=VALUE\0 */

    if (idx >= 0) {
        int old_exp = env_tab[idx].exported;
        for (int i = idx; i < env_count - 1; i++)
            env_copy(i, i + 1);
        env_count--;
        if (exported == -1) exported = old_exp;
    } else if (exported == -1) {
        exported = 0;
    }

    if (env_count >= ENV_MAX) return -1;

    if (env_pool_used + need > ENV_POOL_SIZE) {
        env_compact();
        if (env_pool_used + need > ENV_POOL_SIZE)
            return -1;
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

static void env_unset(const char *key)
{
    int idx = env_find(key);
    if (idx < 0) return;
    for (int i = idx; i < env_count - 1; i++)
        env_copy(i, i + 1);
    env_count--;
}

static int build_envp(char **envp, int max)
{
    int n = 0;
    for (int i = 0; i < env_count && n < max - 1; i++) {
        if (env_tab[i].exported)
            envp[n++] = env_pool + env_tab[i].off;
    }
    envp[n] = 0;
    return n;
}

static void env_init(char **envp)
{
    if (!envp) return;
    for (int i = 0; envp[i] && env_count < ENV_MAX; i++) {
        const char *s = envp[i];
        int len = my_strlen(s) + 1;
        if (env_pool_used + len > ENV_POOL_SIZE) break;
        env_tab[env_count].off = env_pool_used;
        env_tab[env_count].exported = 1;
        for (int j = 0; j < len; j++)
            env_pool[env_pool_used + j] = s[j];
        env_pool_used += len;
        env_count++;
    }
}

/* ── Variable expansion helper ───────────────────────────────────────── */

static void expand_var(const char **pp, char **outp, char *end)
{
    const char *p = *pp + 1;  /* skip '$' */

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
    } else if (*p == '0') {
        if (shell_name)
            for (int i = 0; shell_name[i] && *outp < end; i++)
                *(*outp)++ = shell_name[i];
        p++;
    } else {
        /* Unknown — emit literal '$' */
        if (*outp < end) *(*outp)++ = '$';
    }

    *pp = p;
}

/* ── Tokenizer ───────────────────────────────────────────────────────── */

static int is_op_start(char c)
{
    return c == '|' || c == '&' || c == ';' || c == '>' || c == '<';
}

/*
 * Tokenize input line into buf, producing token pointers in toks[].
 * Operator tokens point to string literals (not into buf).
 * Word tokens point into buf (NUL-terminated, with $VAR expanded).
 */
static int tokenize(const char *input, char *buf, int buf_size,
                    char **toks, int max_toks)
{
    const char *p = input;
    char *out = buf;
    char *end = buf + buf_size - 1;
    int n = 0;

#define EMIT(c) do { if (out < end) *out++ = (c); } while(0)

    while (*p && n < max_toks) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') break;

        /* Multi-char operators (check longest first) */
        if (p[0] == '2' && p[1] == '>' && p[2] == '>')
            { toks[n++] = "2>>"; p += 3; continue; }
        if (p[0] == '2' && p[1] == '>' && p[2] == '&' && p[3] == '1')
            { toks[n++] = "2>&1"; p += 4; continue; }
        if (p[0] == '1' && p[1] == '>' && p[2] == '&' && p[3] == '2')
            { toks[n++] = "1>&2"; p += 4; continue; }
        if (p[0] == '1' && p[1] == '>' && p[2] == '>')
            { toks[n++] = ">>"; p += 3; continue; }
        if (p[0] == '2' && p[1] == '>')
            { toks[n++] = "2>"; p += 2; continue; }
        if (p[0] == '1' && p[1] == '>')
            { toks[n++] = ">"; p += 2; continue; }
        if (p[0] == '>' && p[1] == '>')
            { toks[n++] = ">>"; p += 2; continue; }
        if (p[0] == '&' && p[1] == '&')
            { toks[n++] = "&&"; p += 2; continue; }
        if (p[0] == '|' && p[1] == '|')
            { toks[n++] = "||"; p += 2; continue; }

        /* Single-char operators */
        if (*p == '>') { toks[n++] = ">"; p++; continue; }
        if (*p == '<') { toks[n++] = "<"; p++; continue; }
        if (*p == '|') { toks[n++] = "|"; p++; continue; }
        if (*p == ';') { toks[n++] = ";"; p++; continue; }

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
static int match_score(const char *want, const char *have)
{
    if (streq(want, have)) return 1;
    if (strcaseeq(want, have)) return 2;
    const char *dot = my_strrchr(have, '.');
    if (dot) {
        char base[PPAP_NAME_MAX + 1];
        my_strncpy(base, have, (int)(dot - have), sizeof(base));
        if (streq(want, base)) return 3;
        if (strcaseeq(want, base)) return 4;
    }
    return 5;
}

static void build_path(char *dst, int size, const char *dir, const char *name)
{
    int i = 0;
    for (int j = 0; dir[j] && i < size - 2; j++) dst[i++] = dir[j];
    if (i > 0 && dst[i - 1] != '/') dst[i++] = '/';
    for (int j = 0; name[j] && i < size - 1; j++) dst[i++] = name[j];
    dst[i] = '\0';
}

static int search_path(const char *name, char *result, int rsize)
{
    if (my_strchr(name, '/')) {
        my_strcpy(result, name, rsize);
        return 0;
    }

    const char *path = env_get("PATH");
    if (!path) path = "/bin:/sbin";

    while (*path) {
        const char *sep = path;
        while (*sep && *sep != ':') sep++;

        char dir[PATH_BUF];
        int dlen = (int)(sep - path);
        if (dlen == 0) {
            dir[0] = '.'; dir[1] = '\0';
        } else {
            my_strncpy(dir, path, dlen, sizeof(dir));
        }

        int best_score = 5;
        char best_name[PPAP_NAME_MAX + 1];

        int dfd = open(dir, O_RDONLY, 0);
        if (dfd >= 0) {
            struct dirent de;
            while (getdents(dfd, &de, sizeof(de)) > 0) {
                if (de.d_type == DT_DIR) continue;
                int score = match_score(name, de.d_name);
                if (score < best_score) {
                    best_score = score;
                    my_strcpy(best_name, de.d_name, sizeof(best_name));
                    if (score == 1) break;
                }
            }
            close(dfd);
        }

        if (best_score < 5) {
            build_path(result, rsize, dir, best_name);
            return 0;
        }

        path = *sep ? sep + 1 : sep;
    }
    return -1;
}

/* ── [[ ]] builtin test evaluator ────────────────────────────────────── */

static char **tt;
static int    tt_n;
static int    tt_pos;

static int t_expr(void);

static int t_primary(void)
{
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
        case 'e': return stat(arg, &st) == 0;
        case 'f': return stat(arg, &st) == 0 && S_ISREG(st.st_mode);
        case 'd': return stat(arg, &st) == 0 && S_ISDIR(st.st_mode);
        case 'r': return access(arg, R_OK) == 0;
        case 'w': return access(arg, W_OK) == 0;
        case 'x': return access(arg, X_OK) == 0;
        case 's': return stat(arg, &st) == 0 && st.st_size > 0;
        case 'z': return my_strlen(arg) == 0;
        case 'n': return my_strlen(arg) > 0;
        }
        return 0;
    }

    /* Binary: left OP right */
    if (tt_pos + 2 < tt_n) {
        const char *op = tt[tt_pos + 1];
        if (streq(op, "=") || streq(op, "!=") ||
            streq(op, "-eq") || streq(op, "-ne") ||
            streq(op, "-lt") || streq(op, "-gt") ||
            streq(op, "-le") || streq(op, "-ge"))
        {
            const char *left  = tt[tt_pos];
            const char *right = tt[tt_pos + 2];
            tt_pos += 3;
            if (streq(op, "="))   return streq(left, right);
            if (streq(op, "!="))  return !streq(left, right);
            int l = my_atoi(left), r = my_atoi(right);
            if (streq(op, "-eq")) return l == r;
            if (streq(op, "-ne")) return l != r;
            if (streq(op, "-lt")) return l < r;
            if (streq(op, "-gt")) return l > r;
            if (streq(op, "-le")) return l <= r;
            return l >= r;  /* -ge */
        }
    }

    /* Bare word: true if non-empty */
    tt_pos++;
    return tok[0] != '\0';
}

static int t_not(void)
{
    if (tt_pos < tt_n && streq(tt[tt_pos], "!")) {
        tt_pos++;
        return !t_not();
    }
    return t_primary();
}

static int t_and(void)
{
    int r = t_not();
    while (tt_pos < tt_n && streq(tt[tt_pos], "&&")) {
        tt_pos++;
        int rr = t_not();
        r = r && rr;
    }
    return r;
}

static int t_expr(void)
{
    int r = t_and();
    while (tt_pos < tt_n && streq(tt[tt_pos], "||")) {
        tt_pos++;
        int rr = t_and();
        r = r || rr;
    }
    return r;
}

/* Returns shell exit code: 0 = true, 1 = false */
static int eval_test(char **tokens, int count)
{
    tt = tokens;
    tt_n = count;
    tt_pos = 0;
    return t_expr() ? 0 : 1;
}

/* ── Redirect parsing ────────────────────────────────────────────────── */

struct redir {
    const char *file;   /* NULL for fd dup */
    int target_fd;      /* fd to redirect */
    int flags;          /* open flags */
    int source_fd;      /* >= 0 for fd dup, -1 for file */
};

static int parse_redirects(char **argv, int argc,
                           struct redir *rr, int *nrr)
{
    *nrr = 0;
    int out = 0;
    for (int i = 0; i < argc; i++) {
        if (*nrr >= REDIR_MAX) { argv[out++] = argv[i]; continue; }

        if (streq(argv[i], ">") && i + 1 < argc) {
            rr[*nrr].file = argv[++i]; rr[*nrr].target_fd = 1;
            rr[*nrr].flags = O_WRONLY | O_CREAT | O_TRUNC;
            rr[(*nrr)++].source_fd = -1;
        } else if (streq(argv[i], ">>") && i + 1 < argc) {
            rr[*nrr].file = argv[++i]; rr[*nrr].target_fd = 1;
            rr[*nrr].flags = O_WRONLY | O_CREAT | O_APPEND;
            rr[(*nrr)++].source_fd = -1;
        } else if (streq(argv[i], "<") && i + 1 < argc) {
            rr[*nrr].file = argv[++i]; rr[*nrr].target_fd = 0;
            rr[*nrr].flags = O_RDONLY;
            rr[(*nrr)++].source_fd = -1;
        } else if (streq(argv[i], "2>") && i + 1 < argc) {
            rr[*nrr].file = argv[++i]; rr[*nrr].target_fd = 2;
            rr[*nrr].flags = O_WRONLY | O_CREAT | O_TRUNC;
            rr[(*nrr)++].source_fd = -1;
        } else if (streq(argv[i], "2>>") && i + 1 < argc) {
            rr[*nrr].file = argv[++i]; rr[*nrr].target_fd = 2;
            rr[*nrr].flags = O_WRONLY | O_CREAT | O_APPEND;
            rr[(*nrr)++].source_fd = -1;
        } else if (streq(argv[i], "2>&1")) {
            rr[*nrr].file = 0; rr[*nrr].target_fd = 2;
            rr[*nrr].flags = 0; rr[(*nrr)++].source_fd = 1;
        } else if (streq(argv[i], "1>&2")) {
            rr[*nrr].file = 0; rr[*nrr].target_fd = 1;
            rr[*nrr].flags = 0; rr[(*nrr)++].source_fd = 2;
        } else {
            argv[out++] = argv[i];
        }
    }
    argv[out] = 0;
    return out;
}

static void apply_redirects(struct redir *rr, int nrr)
{
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
static void save_fds(struct redir *rr, int nrr, int saved[3])
{
    saved[0] = saved[1] = saved[2] = -1;
    for (int i = 0; i < nrr; i++) {
        int tfd = rr[i].target_fd;
        if (tfd < 3 && saved[tfd] < 0)
            saved[tfd] = dup(tfd);
    }
    apply_redirects(rr, nrr);
}

static void restore_fds(int saved[3])
{
    for (int i = 0; i < 3; i++) {
        if (saved[i] >= 0) {
            dup2(saved[i], i);
            close(saved[i]);
        }
    }
}

/* ── Builtins ────────────────────────────────────────────────────────── */

static int is_builtin(const char *cmd)
{
    return streq(cmd, "exit") || streq(cmd, "true") || streq(cmd, "false") ||
           streq(cmd, "cd")   || streq(cmd, "pwd")  || streq(cmd, "echo") ||
           streq(cmd, "export") || streq(cmd, "unset") || streq(cmd, "set") ||
           streq(cmd, "env") || streq(cmd, ".") || streq(cmd, "source");
}

/* Execute builtin.  Always returns 1 (handled).  Sets *status. */
static void run_builtin(char **argv, int argc, int *status)
{
    const char *cmd = argv[0];

    if (streq(cmd, "exit"))
        _exit(argc > 1 ? my_atoi(argv[1]) : last_status);

    if (streq(cmd, "true"))  { *status = 0; return; }
    if (streq(cmd, "false")) { *status = 1; return; }

    if (streq(cmd, "cd")) {
        const char *dir = argc > 1 ? argv[1] : env_get("HOME");
        if (!dir) dir = "/";
        if (streq(dir, "-")) {
            dir = env_get("OLDPWD");
            if (!dir) { err_msg("cd", "OLDPWD not set"); *status = 1; return; }
        }
        char old[PATH_BUF];
        if (getcwd(old, sizeof(old)) == 0)
            env_set("OLDPWD", old, -1);
        if (chdir(dir) < 0) {
            err_msg("cd", dir);
            *status = 1;
        } else {
            char cwd[PATH_BUF];
            if (getcwd(cwd, sizeof(cwd)) == 0)
                env_set("PWD", cwd, -1);
            *status = 0;
        }
        return;
    }

    if (streq(cmd, "pwd")) {
        char cwd[PATH_BUF];
        if (getcwd(cwd, sizeof(cwd)) == 0) {
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
            if (i == 1 && streq(argv[i], "-n")) { no_nl = 1; continue; }
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
        for (int i = 1; i < argc; i++)
            env_unset(argv[i]);
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

    *status = 127;
}

/* ── Simple command execution ────────────────────────────────────────── */

static int is_valid_ident_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int exec_simple(char **argv, int argc)
{
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
                if (!is_valid_ident_char(*p)) { valid = 0; break; }
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
            if (streq(argv[i], "]]")) { end = i; break; }
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

    /* External command — resolve via PATH */
    char resolved[PATH_BUF];
    if (search_path(argv[0], resolved, sizeof(resolved)) < 0) {
        err_msg(argv[0], "not found");
        return 127;
    }

    char *envp[ENV_MAX + 1];
    build_envp(envp, ENV_MAX + 1);

    pid_t pid = vfork();
    if (pid == 0) {
        apply_redirects(redirs, nredirs);
        execve(resolved, argv, envp);
        _exit(127);
    }
    if (pid < 0) {
        err_msg(argv[0], "fork failed");
        return 1;
    }

    int wstatus;
    waitpid(pid, &wstatus, 0);
    return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 128 + WTERMSIG(wstatus);
}

/* ── Pipeline execution ──────────────────────────────────────────────── */

static int exec_pipeline(char **toks, int ntoks, int *pos)
{
    struct {
        char *argv[ARGV_MAX + 1];
        int   argc;
    } stages[PIPE_MAX];
    int nstages = 0;
    int depth = 0;  /* [[ ]] nesting depth */

    stages[0].argc = 0;

    while (*pos < ntoks) {
        const char *t = toks[*pos];

        if (streq(t, "[[")) depth++;
        else if (streq(t, "]]")) { if (depth > 0) depth--; }

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

    for (int i = 0; i < nstages; i++)
        stages[i].argv[stages[i].argc] = 0;

    /* Single command — no pipe overhead */
    if (nstages == 1)
        return exec_simple(stages[0].argv, stages[0].argc);

    /* Multi-stage pipeline */
    char *envp[ENV_MAX + 1];
    build_envp(envp, ENV_MAX + 1);

    pid_t pids[PIPE_MAX];
    int prev_read = -1;

    for (int i = 0; i < nstages; i++) {
        int pipefd[2] = { -1, -1 };
        if (i < nstages - 1) {
            if (pipe(pipefd) < 0) {
                err_msg("pipe", "failed");
                return 1;
            }
        }

        struct redir redirs[REDIR_MAX];
        int nredirs;
        stages[i].argc = parse_redirects(stages[i].argv, stages[i].argc,
                                         redirs, &nredirs);

        char resolved[PATH_BUF];
        int has_path = 0;
        if (stages[i].argc > 0)
            has_path = (search_path(stages[i].argv[0], resolved,
                                    sizeof(resolved)) == 0);

        pid_t pid = vfork();
        if (pid == 0) {
            if (prev_read >= 0) { dup2(prev_read, 0); close(prev_read); }
            if (pipefd[1] >= 0) { dup2(pipefd[1], 1); close(pipefd[1]); }
            if (pipefd[0] >= 0) close(pipefd[0]);
            apply_redirects(redirs, nredirs);
            if (has_path && stages[i].argc > 0)
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
    for (int i = 0; i < nstages; i++) {
        int wstatus;
        waitpid(pids[i], &wstatus, 0);
        if (i == nstages - 1)
            status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus)
                                        : 128 + WTERMSIG(wstatus);
    }
    return status;
}

/* ── List execution (&&, ||, ;) ──────────────────────────────────────── */

/* Skip one pipeline's worth of tokens */
static void skip_pipeline(char **toks, int ntoks, int *pos)
{
    int depth = 0;
    while (*pos < ntoks) {
        if (streq(toks[*pos], "[[")) depth++;
        else if (streq(toks[*pos], "]]")) { if (depth > 0) depth--; }
        else if (depth == 0) {
            if (streq(toks[*pos], ";") || streq(toks[*pos], "&&") ||
                streq(toks[*pos], "||"))
                return;
        }
        (*pos)++;
    }
}

static int exec_list(char **toks, int ntoks)
{
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
                break;  /* next segment */
            }
            if (streq(toks[pos], "&&")) {
                pos++;
                if (status != 0) {
                    skip_pipeline(toks, ntoks, &pos);
                } else {
                    break;  /* execute next pipeline */
                }
                continue;
            }
            if (streq(toks[pos], "||")) {
                pos++;
                if (status == 0) {
                    skip_pipeline(toks, ntoks, &pos);
                } else {
                    break;  /* execute next pipeline */
                }
                continue;
            }
            break;  /* not a chain op, done */
        }
    }
    return status;
}

/* ── Line execution ──────────────────────────────────────────────────── */

static int execute_line(const char *line)
{
    char buf[TOK_BUF_SIZE];
    char *toks[TOKEN_MAX];
    int ntoks = tokenize(line, buf, sizeof(buf), toks, TOKEN_MAX);
    if (ntoks <= 0) return 0;

    int status = exec_list(toks, ntoks);
    last_status = status;
    return status;
}

/* ── File reader ─────────────────────────────────────────────────────── */

static int read_line(int fd, char *buf, int len)
{
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

static int run_file(const char *path)
{
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

    char line[PUSH_LINE_MAX];
    int status = 0;
    int first = 1;

    while (read_line(fd, line, sizeof(line)) >= 0) {
        /* Skip shebang on first line */
        if (first && line[0] == '#' && line[1] == '!')
            { first = 0; continue; }
        first = 0;
        if (line[0] == '\0') continue;
        status = execute_line(line);
    }

    if (close_fd) close(fd);
    return status;
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    shell_pid = getpid();
    shell_name = argc > 0 ? argv[0] : "push";

    /* Import environment from parent (envp follows argv on stack) */
    if (argc > 0 && argv[argc] == 0) {
        char **envp = &argv[argc + 1];
        env_init(envp);
    }

    /* Initialize PWD */
    char cwd[PATH_BUF];
    if (getcwd(cwd, sizeof(cwd)) == 0)
        env_set("PWD", cwd, 1);

    /* Script mode: push script.sh [args...] */
    if (argc > 1) {
        shell_name = argv[1];
        return run_file(argv[1]);
    }

    /* Interactive mode (minimal — Phase 3 adds line editing) */
    char line[PUSH_LINE_MAX];
    for (;;) {
        puts_fd(2, "$ ");
        if (read_line(0, line, sizeof(line)) < 0)
            break;
        if (line[0] == '\0') continue;
        execute_line(line);
    }
    write(2, "\n", 1);
    return last_status;
}
