#include "pdb_util.h"

#define PDB_SCRIPT_CMD_MAX  32
#define PDB_SCRIPT_LINE_MAX  128
#define PDB_SCRIPT_BUF_MAX  2048

void put_str(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    write(1, s, (size_t)n);
}

void put_err(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    write(2, s, (size_t)n);
}

void put_chr(char c)
{
    write(1, &c, 1);
}

void put_u32(uint32_t v)
{
    static const uint32_t pw[] = {
        1000000000u, 100000000u, 10000000u, 1000000u,
        100000u, 10000u, 1000u, 100u, 10u, 1u
    };
    int started = 0;

    if (v == 0) {
        put_chr('0');
        return;
    }

    for (int i = 0; i < 10; i++) {
        uint32_t d = 0;
        while (v >= pw[i]) {
            v -= pw[i];
            d++;
        }
        if (d || started) {
            put_chr((char)('0' + d));
            started = 1;
        }
    }
}

void put_i32(int32_t v)
{
    if (v < 0) {
        put_chr('-');
        put_u32((uint32_t)(-v));
        return;
    }
    put_u32((uint32_t)v);
}

void put_hex32(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    put_str("0x");
    for (int s = 28; s >= 0; s -= 4)
        put_chr(hex[(v >> s) & 0xf]);
}

void put_hex16(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    put_str("0x");
    for (int s = 12; s >= 0; s -= 4)
        put_chr(hex[(v >> s) & 0xf]);
}

void put_hex8(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    put_str("0x");
    put_chr(hex[(v >> 4) & 0xf]);
    put_chr(hex[v & 0xf]);
}

uint32_t select_bp_flag_from_caps(uint32_t caps_bits)
{
    if (caps_bits & PPAP_PTRACE_CAP_SW_BP)
        return PPAP_PTRACE_BP_SW;
    if (caps_bits & PPAP_PTRACE_CAP_HW_BP)
        return PPAP_PTRACE_BP_HW;
    return 0;
}

const char *bp_flag_name(uint32_t flags)
{
    if (flags & PPAP_PTRACE_BP_SW)
        return "sw";
    if (flags & PPAP_PTRACE_BP_HW)
        return "hw";
    return "unknown";
}

int streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int readline(char *buf, int size)
{
    int n = 0;

    if (size <= 1)
        return -1;

    for (;;) {
        char c = 0;
        ssize_t rc = read(0, &c, 1);
        if (rc <= 0)
            return -1;
        if (c == '\r')
            continue;
        if (c == '\n')
            break;
        if (n < size - 1)
            buf[n++] = c;
    }

    buf[n] = '\0';
    return n;
}

int split_tokens(char *line, char **tok, int max_tok)
{
    int n = 0;

    while (*line && n < max_tok) {
        while (*line == ' ' || *line == '\t')
            line++;
        if (!*line)
            break;
        tok[n++] = line;
        while (*line && *line != ' ' && *line != '\t')
            line++;
        if (*line)
            *line++ = '\0';
    }

    return n;
}

static int append_script_cmd(char **script_cmds, int *script_count,
                             char *storage, int *storage_used,
                             const char *cmd)
{
    int len = 0;

    if (*script_count >= PDB_SCRIPT_CMD_MAX)
        return -1;
    while (cmd[len])
        len++;
    if (*storage_used + len + 1 > PDB_SCRIPT_BUF_MAX)
        return -1;

    for (int i = 0; i < len; i++)
        storage[*storage_used + i] = cmd[i];
    storage[*storage_used + len] = '\0';
    script_cmds[*script_count] = &storage[*storage_used];
    *storage_used += len + 1;
    (*script_count)++;
    return 0;
}

int is_script_space(char c)
{
    return c == ' ' || c == '\t';
}

static int append_script_line(char **script_cmds, int *script_count,
                              char *storage, int *storage_used,
                              char *line, int len)
{
    int start = 0;
    int end = len;

    while (start < len && is_script_space(line[start]))
        start++;
    while (end > start && is_script_space(line[end - 1]))
        end--;
    if (end <= start)
        return 0; /* blank line */
    if (line[start] == '#')
        return 0; /* comment line */

    line[end] = '\0';
    return append_script_cmd(script_cmds, script_count,
                             storage, storage_used, &line[start]);
}

int load_script_file(const char *path, char **script_cmds,
                     int *script_count, char *storage,
                     int *storage_used)
{
    int fd = open(path, O_RDONLY, 0);
    char line[PDB_SCRIPT_LINE_MAX];
    int len = 0;
    int dropping = 0;

    if (fd < 0) {
        put_err("pdb: cannot open script file: ");
        put_err(path);
        put_chr('\n');
        return -1;
    }

    for (;;) {
        char c = 0;
        int n = read(fd, &c, 1);
        if (n < 0) {
            put_err("pdb: failed to read script file: ");
            put_err(path);
            put_chr('\n');
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        if (c == '\r')
            continue;
        if (c == '\n') {
            if (dropping) {
                put_err("pdb: script line too long in ");
                put_err(path);
                put_chr('\n');
                close(fd);
                return -1;
            }
            if (append_script_line(script_cmds, script_count,
                                   storage, storage_used, line, len) < 0) {
                put_err("pdb: script command limit exceeded while reading ");
                put_err(path);
                put_chr('\n');
                close(fd);
                return -1;
            }
            len = 0;
            dropping = 0;
            continue;
        }
        if (dropping)
            continue;
        if (len >= PDB_SCRIPT_LINE_MAX - 1) {
            dropping = 1;
            continue;
        }
        line[len++] = c;
    }

    if (dropping) {
        put_err("pdb: script line too long in ");
        put_err(path);
        put_chr('\n');
        close(fd);
        return -1;
    }

    if (append_script_line(script_cmds, script_count,
                           storage, storage_used, line, len) < 0) {
        put_err("pdb: script command limit exceeded while reading ");
        put_err(path);
        put_chr('\n');
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int parse_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int base = 10;
    int seen = 0;

    if (!s || !*s)
        return 0;

    if (s[0] == '$') {
        base = 16;
        s++;
    } else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }

    while (*s) {
        char c = *s++;
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A' + 10);
        } else {
            return 0;
        }
        if (d >= (uint32_t)base)
            return 0;
        v = v * (uint32_t)base + d;
        seen = 1;
    }

    if (!seen)
        return 0;

    *out = v;
    return 1;
}

int parse_x_spec(const char *tok0, uint32_t *count_out, char *fmt_out)
{
    const char *p;
    uint32_t count = 0;
    int has_digits = 0;
    char fmt = 'x';

    if (!tok0 || tok0[0] != 'x' || tok0[1] != '/')
        return 0;

    p = tok0 + 2;
    while (*p >= '0' && *p <= '9') {
        has_digits = 1;
        count = count * 10u + (uint32_t)(*p - '0');
        p++;
    }
    if (!has_digits)
        return 0;
    if (*p == 'x' || *p == 'b' || *p == 'h') {
        fmt = *p;
        p++;
    }
    if (*p != '\0')
        return 0;
    *count_out = count;
    *fmt_out = fmt;
    return 1;
}

int parse_mem_width(const char *tok, uint32_t *width_out)
{
    if (!tok || !*tok || streq(tok, "w") || streq(tok, "word") || streq(tok, "4")) {
        *width_out = 4u;
        return 1;
    }
    if (streq(tok, "h") || streq(tok, "half") || streq(tok, "2")) {
        *width_out = 2u;
        return 1;
    }
    if (streq(tok, "b") || streq(tok, "byte") || streq(tok, "1")) {
        *width_out = 1u;
        return 1;
    }
    return 0;
}

int parse_surface_token(const char *token, uint32_t *surface_out)
{
    if (streq(token, "real")) {
        *surface_out = PPAP_TRACE_SURFACE_REAL;
        return 1;
    }
    if (streq(token, "ecpu")) {
        *surface_out = PPAP_TRACE_SURFACE_ECPU;
        return 1;
    }
    return 0;
}
