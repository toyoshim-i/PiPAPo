#include "syscall.h"

#define PDB_LOCAL_BP_MAX  32
#define PDB_SCRIPT_CMD_MAX  32
#define PDB_SCRIPT_LINE_MAX  128
#define PDB_SCRIPT_BUF_MAX  2048

typedef struct {
    uint8_t used;
    uint8_t enabled;
    uint32_t addr;
} pdb_local_bp_t;

static char pdb_script_storage[PDB_SCRIPT_BUF_MAX];

static void put_str(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    write(1, s, (size_t)n);
}

static void put_err(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    write(2, s, (size_t)n);
}

static void put_chr(char c)
{
    write(1, &c, 1);
}

static void put_u32(uint32_t v)
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

static void put_i32(int32_t v)
{
    if (v < 0) {
        put_chr('-');
        put_u32((uint32_t)(-v));
        return;
    }
    put_u32((uint32_t)v);
}

static void put_hex32(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    put_str("0x");
    for (int s = 28; s >= 0; s -= 4)
        put_chr(hex[(v >> s) & 0xf]);
}

static void put_hex16(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    put_str("0x");
    for (int s = 12; s >= 0; s -= 4)
        put_chr(hex[(v >> s) & 0xf]);
}

static void put_hex8(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    put_str("0x");
    put_chr(hex[(v >> 4) & 0xf]);
    put_chr(hex[v & 0xf]);
}

static int streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int readline(char *buf, int size)
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

static int split_tokens(char *line, char **tok, int max_tok)
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

static int is_script_space(char c)
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

static int load_script_file(const char *path, char **script_cmds,
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

static int parse_u32(const char *s, uint32_t *out)
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

static int parse_x_spec(const char *tok0, uint32_t *count_out, char *fmt_out)
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

static int parse_mem_width(const char *tok, uint32_t *width_out)
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

static const char *event_name(uint32_t ev)
{
    switch (ev) {
    case PPAP_TRACE_EVENT_NONE: return "none";
    case PPAP_TRACE_EVENT_EXEC: return "exec";
    case PPAP_TRACE_EVENT_SYSCALL_ENTER: return "sys-enter";
    case PPAP_TRACE_EVENT_SYSCALL_EXIT: return "sys-exit";
    case PPAP_TRACE_EVENT_SUBSYS_ENTER: return "subsys-enter";
    case PPAP_TRACE_EVENT_SUBSYS_EXIT: return "subsys-exit";
    case PPAP_TRACE_EVENT_DEBUG_STOP: return "debug-stop";
    default: return "event";
    }
}

static const char *abi_name(uint32_t abi)
{
    switch (abi) {
    case PPAP_TRACE_ABI_PPAP: return "ppap";
    case PPAP_TRACE_ABI_H68K_DOS: return "h68k-dos";
    case PPAP_TRACE_ABI_H68K_IOCS: return "h68k-iocs";
    case PPAP_TRACE_ABI_CPM_BDOS: return "cpm-bdos";
    case PPAP_TRACE_ABI_CPM_BIOS: return "cpm-bios";
    default: return "abi";
    }
}

static const char *regset_name(uint32_t regset)
{
    switch (regset) {
    case PPAP_TRACE_REGSET_ARM: return "arm";
    case PPAP_TRACE_REGSET_M68K: return "m68k";
    case PPAP_TRACE_REGSET_Z80: return "z80";
    default: return "unknown";
    }
}

static const char *surface_name_for_regset(uint32_t regset)
{
    if (regset == PPAP_TRACE_REGSET_Z80)
        return "ecpu";
    if (regset == PPAP_TRACE_REGSET_NONE)
        return "unknown";
    return "real";
}

static const char *surface_name_for_value(uint32_t surface)
{
    switch (surface) {
    case PPAP_TRACE_SURFACE_REAL:
        return "real";
    case PPAP_TRACE_SURFACE_ECPU:
        return "ecpu";
    default:
        return "unknown";
    }
}

static int parse_surface_token(const char *token, uint32_t *surface_out)
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

static void print_surface_mask(uint32_t surfaces)
{
    int has = 0;
    if (surfaces & PPAP_PTRACE_SURFACE_MASK_REAL) {
        put_str("real");
        has = 1;
    }
    if (surfaces & PPAP_PTRACE_SURFACE_MASK_ECPU) {
        if (has)
            put_chr('|');
        put_str("ecpu");
        has = 1;
    }
    if (!has)
        put_str("none");
}

static void print_caps_bits(uint32_t caps_bits)
{
    int has = 0;

    if (caps_bits & PPAP_PTRACE_CAP_GETREGS) {
        put_str("getregs");
        has = 1;
    }
    if (caps_bits & PPAP_PTRACE_CAP_SETREGS) {
        put_str(has ? "|setregs" : "setregs");
        has = 1;
    }
    if (caps_bits & PPAP_PTRACE_CAP_PEEKPOKE) {
        put_str(has ? "|peekpoke" : "peekpoke");
        has = 1;
    }
    if (caps_bits & PPAP_PTRACE_CAP_SINGLESTEP) {
        put_str(has ? "|step" : "step");
        has = 1;
    }
    if (caps_bits & PPAP_PTRACE_CAP_SW_BP) {
        put_str(has ? "|sw-bp" : "sw-bp");
        has = 1;
    }
    if (caps_bits & PPAP_PTRACE_CAP_HW_BP) {
        put_str(has ? "|hw-bp" : "hw-bp");
        has = 1;
    }
    if (!has)
        put_str("none");
}

static void print_event(const struct ppap_ptrace_event *ev)
{
    put_str("stop ");
    put_str(event_name(ev->event));
    put_str(" abi=");
    put_str(abi_name(ev->abi));

    if (ev->event == PPAP_TRACE_EVENT_DEBUG_STOP) {
        int has_reason = 0;
        put_str(" pc=");
        put_hex32(ev->args[0]);
        put_str(" flags=");
        put_hex32(ev->flags);
        put_str(" reason=");
        if (ev->flags & PPAP_DEBUG_STOP_STEP) {
            put_str("step");
            has_reason = 1;
        }
        if (ev->flags & PPAP_DEBUG_STOP_SW_BP) {
            if (has_reason)
                put_chr('|');
            put_str("sw-bp");
            has_reason = 1;
        }
        if (ev->flags & PPAP_DEBUG_STOP_HW_BP) {
            if (has_reason)
                put_chr('|');
            put_str("hw-bp");
            has_reason = 1;
        }
        if (!has_reason)
            put_str("unknown");
    } else {
        put_str(" nr=");
        put_u32(ev->nr);
        put_str(" ret=");
        put_i32(ev->ret);
    }
    put_chr('\n');
}

static void print_caps(const struct ppap_ptrace_caps *caps)
{
    put_str("regset=");
    put_str(regset_name(caps->regset));
    put_str(" abi=");
    put_str(abi_name(caps->abi));
    put_str(" surface=");
    put_str(surface_name_for_value(caps->surface));
    put_str(" surfaces=");
    print_surface_mask(caps->surfaces);
    put_str(" caps=");
    put_hex32(caps->caps);
    put_str(" [");
    print_caps_bits(caps->caps);
    put_chr(']');
    put_str(" max_bps=");
    put_u32(caps->max_bps);
    put_chr('\n');
}

static uint32_t reg_count(uint32_t regset)
{
    switch (regset) {
    case PPAP_TRACE_REGSET_ARM: return 17;
    case PPAP_TRACE_REGSET_M68K: return 20;
    case PPAP_TRACE_REGSET_Z80: return 18;
    default: return 0;
    }
}

static int reg_is16(uint32_t regset)
{
    return regset == PPAP_TRACE_REGSET_Z80;
}

static const char *reg_name(uint32_t regset, uint32_t idx)
{
    if (regset == PPAP_TRACE_REGSET_ARM) {
        switch (idx) {
        case 0: return "r0";
        case 1: return "r1";
        case 2: return "r2";
        case 3: return "r3";
        case 4: return "r4";
        case 5: return "r5";
        case 6: return "r6";
        case 7: return "r7";
        case 8: return "r8";
        case 9: return "r9";
        case 10: return "r10";
        case 11: return "r11";
        case 12: return "r12";
        case 13: return "sp";
        case 14: return "lr";
        case 15: return "pc";
        case 16: return "xpsr";
        default: return (const char *)0;
        }
    } else if (regset == PPAP_TRACE_REGSET_M68K) {
        switch (idx) {
        case 0: return "d0";
        case 1: return "d1";
        case 2: return "d2";
        case 3: return "d3";
        case 4: return "d4";
        case 5: return "d5";
        case 6: return "d6";
        case 7: return "d7";
        case 8: return "a0";
        case 9: return "a1";
        case 10: return "a2";
        case 11: return "a3";
        case 12: return "a4";
        case 13: return "a5";
        case 14: return "a6";
        case 15: return "a7";
        case 16: return "pc";
        case 17: return "sr";
        case 18: return "usp";
        case 19: return "ksp";
        default: return (const char *)0;
        }
    } else if (regset == PPAP_TRACE_REGSET_Z80) {
        switch (idx) {
        case 0: return "af";
        case 1: return "bc";
        case 2: return "de";
        case 3: return "hl";
        case 4: return "ix";
        case 5: return "iy";
        case 6: return "sp";
        case 7: return "pc";
        case 8: return "af2";
        case 9: return "bc2";
        case 10: return "de2";
        case 11: return "hl2";
        case 12: return "iff1";
        case 13: return "iff2";
        case 14: return "im";
        case 15: return "wz";
        case 16: return "i";
        case 17: return "r";
        default: return (const char *)0;
        }
    }
    return (const char *)0;
}

static int reg_index_from_token(const struct ppap_ptrace_regs *regs,
                                const char *tok, uint32_t *idx_out)
{
    uint32_t idx = 0;
    uint32_t count = reg_count(regs->regset);

    if (parse_u32(tok, &idx)) {
        if (idx >= regs->words)
            return 0;
        *idx_out = idx;
        return 1;
    }

    if (count > regs->words)
        count = regs->words;
    for (idx = 0; idx < count; idx++) {
        const char *name = reg_name(regs->regset, idx);
        if (name && streq(tok, name)) {
            *idx_out = idx;
            return 1;
        }
    }
    return 0;
}

static void print_regs(const struct ppap_ptrace_regs *regs)
{
    int is16 = reg_is16(regs->regset);
    uint32_t count = reg_count(regs->regset);

    put_str("regset=");
    put_str(regset_name(regs->regset));
    put_str(" abi=");
    put_str(abi_name(regs->abi));
    put_str(" words=");
    put_u32(regs->words);
    put_chr('\n');

    if (count > regs->words)
        count = regs->words;
    if (count > PPAP_PTRACE_REGS_MAX)
        count = PPAP_PTRACE_REGS_MAX;
    for (uint32_t i = 0; i < count; i++) {
        const char *name = reg_name(regs->regset, i);
        if (name) {
            put_str(name);
        } else {
            put_str("r");
            put_u32(i);
        }
        put_str("=");
        if (is16)
            put_hex16(regs->regs[i]);
        else
            put_hex32(regs->regs[i]);
        put_chr('\n');
    }
}

static int peek_u8(pid_t pid, uint32_t addr, uint8_t *byte_out);
static int peek_u16le(pid_t pid, uint32_t addr, uint16_t *value_out);
static int peek_u16be(pid_t pid, uint32_t addr, uint16_t *value_out);
static int peek_u32be(pid_t pid, uint32_t addr, uint32_t *value_out);

static void print_mem_words(pid_t pid, uint32_t addr, uint32_t count)
{
    if (count == 0)
        count = 1;
    if (count > 64)
        count = 64;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t word = 0;
        long rc = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)addr, &word);
        if (rc < 0) {
            put_err("pdb: PEEKDATA failed rc=");
            put_i32((int32_t)rc);
            put_chr('\n');
            return;
        }
        put_hex32(addr);
        put_str(": ");
        put_hex32(word);
        put_chr('\n');
        addr += 4;
    }
}

static void print_mem_bytes(pid_t pid, uint32_t addr, uint32_t count)
{
    if (count == 0)
        count = 1;
    if (count > 256)
        count = 256;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t b = 0;
        int rc = peek_u8(pid, addr, &b);
        if (rc < 0) {
            put_err("pdb: PEEKDATA failed rc=");
            put_i32((int32_t)rc);
            put_chr('\n');
            return;
        }
        put_hex32(addr);
        put_str(": ");
        put_hex8((uint32_t)b);
        put_chr('\n');
        addr += 1u;
    }
}

static void print_mem_halfwords(pid_t pid, uint32_t addr, uint32_t count)
{
    if (count == 0)
        count = 1;
    if (count > 128)
        count = 128;

    for (uint32_t i = 0; i < count; i++) {
        uint16_t value = 0;
        int rc = peek_u16le(pid, addr, &value);
        if (rc < 0) {
            put_err("pdb: PEEKDATA failed rc=");
            put_i32((int32_t)rc);
            put_chr('\n');
            return;
        }
        put_hex32(addr);
        put_str(": ");
        put_hex16((uint32_t)value);
        put_chr('\n');
        addr += 2u;
    }
}

static int peek_word(pid_t pid, uint32_t addr, uint32_t *word_out)
{
    uint32_t base = addr & ~3u;
    long rc = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)base, word_out);
    if (rc < 0)
        return (int)rc;
    return 0;
}

static int peek_u8(pid_t pid, uint32_t addr, uint8_t *byte_out)
{
    uint32_t word = 0;
    int rc = peek_word(pid, addr, &word);
    if (rc < 0)
        return rc;
    *byte_out = (uint8_t)((word >> ((addr & 3u) * 8u)) & 0xffu);
    return 0;
}

static int peek_u16le(pid_t pid, uint32_t addr, uint16_t *value_out)
{
    uint8_t lo = 0;
    uint8_t hi = 0;
    int rc = peek_u8(pid, addr, &lo);
    if (rc < 0)
        return rc;
    rc = peek_u8(pid, addr + 1u, &hi);
    if (rc < 0)
        return rc;
    *value_out = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    return 0;
}

static int peek_u16be(pid_t pid, uint32_t addr, uint16_t *value_out)
{
    uint8_t hi = 0;
    uint8_t lo = 0;
    int rc = peek_u8(pid, addr, &hi);
    if (rc < 0)
        return rc;
    rc = peek_u8(pid, addr + 1u, &lo);
    if (rc < 0)
        return rc;
    *value_out = (uint16_t)(((uint16_t)hi << 8) | (uint16_t)lo);
    return 0;
}

static int peek_u32be(pid_t pid, uint32_t addr, uint32_t *value_out)
{
    uint16_t hi = 0;
    uint16_t lo = 0;
    int rc = peek_u16be(pid, addr, &hi);
    if (rc < 0)
        return rc;
    rc = peek_u16be(pid, addr + 2u, &lo);
    if (rc < 0)
        return rc;
    *value_out = ((uint32_t)hi << 16) | (uint32_t)lo;
    return 0;
}

static int z80_disas_one(pid_t pid, uint32_t pc, uint32_t *next_pc)
{
    uint8_t op = 0;
    uint8_t imm8 = 0;
    uint16_t imm16 = 0;
    uint32_t len = 1;
    int rc = peek_u8(pid, pc, &op);
    if (rc < 0)
        return rc;

    put_hex32(pc);
    put_str(": ");
    switch (op) {
    case 0x00:
        put_str("nop");
        len = 1;
        break;
    case 0x06:
        rc = peek_u8(pid, pc + 1u, &imm8);
        if (rc < 0)
            return rc;
        put_str("ld b,#");
        put_hex8(imm8);
        len = 2;
        break;
    case 0x0e:
        rc = peek_u8(pid, pc + 1u, &imm8);
        if (rc < 0)
            return rc;
        put_str("ld c,#");
        put_hex8(imm8);
        len = 2;
        break;
    case 0x16:
        rc = peek_u8(pid, pc + 1u, &imm8);
        if (rc < 0)
            return rc;
        put_str("ld d,#");
        put_hex8(imm8);
        len = 2;
        break;
    case 0x1e:
        rc = peek_u8(pid, pc + 1u, &imm8);
        if (rc < 0)
            return rc;
        put_str("ld e,#");
        put_hex8(imm8);
        len = 2;
        break;
    case 0x26:
        rc = peek_u8(pid, pc + 1u, &imm8);
        if (rc < 0)
            return rc;
        put_str("ld h,#");
        put_hex8(imm8);
        len = 2;
        break;
    case 0x2e:
        rc = peek_u8(pid, pc + 1u, &imm8);
        if (rc < 0)
            return rc;
        put_str("ld l,#");
        put_hex8(imm8);
        len = 2;
        break;
    case 0x3e:
        rc = peek_u8(pid, pc + 1u, &imm8);
        if (rc < 0)
            return rc;
        put_str("ld a,#");
        put_hex8(imm8);
        len = 2;
        break;
    case 0xc3:
        rc = peek_u16le(pid, pc + 1u, &imm16);
        if (rc < 0)
            return rc;
        put_str("jp ");
        put_hex16(imm16);
        len = 3;
        break;
    case 0xc9:
        put_str("ret");
        len = 1;
        break;
    case 0xcd:
        rc = peek_u16le(pid, pc + 1u, &imm16);
        if (rc < 0)
            return rc;
        put_str("call ");
        put_hex16(imm16);
        len = 3;
        break;
    case 0x76:
        put_str("halt");
        len = 1;
        break;
    default:
        put_str("db ");
        put_hex8(op);
        len = 1;
        break;
    }

    put_str(" ;");
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = 0;
        rc = peek_u8(pid, pc + i, &b);
        if (rc < 0)
            return rc;
        put_chr(' ');
        put_hex8(b);
    }
    put_chr('\n');
    *next_pc = pc + len;
    return 0;
}

static void disas_z80(pid_t pid, uint32_t pc, uint32_t count)
{
    if (count == 0)
        count = 8;
    if (count > 64)
        count = 64;

    for (uint32_t i = 0; i < count; i++) {
        int rc = z80_disas_one(pid, pc, &pc);
        if (rc < 0) {
            put_err("pdb: disas failed rc=");
            put_i32((int32_t)rc);
            put_chr('\n');
            return;
        }
    }
}

static const char *m68k_cc_name(uint16_t cc)
{
    switch (cc & 0x0f) {
    case 0x0: return "bra";
    case 0x1: return "bsr";
    case 0x2: return "bhi";
    case 0x3: return "bls";
    case 0x4: return "bcc";
    case 0x5: return "bcs";
    case 0x6: return "bne";
    case 0x7: return "beq";
    case 0x8: return "bvc";
    case 0x9: return "bvs";
    case 0xa: return "bpl";
    case 0xb: return "bmi";
    case 0xc: return "bge";
    case 0xd: return "blt";
    case 0xe: return "bgt";
    default: return "ble";
    }
}

static int m68k_disas_one(pid_t pid, uint32_t pc, uint32_t *next_pc)
{
    uint16_t op = 0;
    uint16_t imm16 = 0;
    uint32_t imm32 = 0;
    uint32_t len = 2;
    int rc = peek_u16be(pid, pc, &op);
    if (rc < 0)
        return rc;

    put_hex32(pc);
    put_str(": ");

    if (op == 0x4e71) {
        put_str("nop");
        len = 2;
    } else if (op == 0x4e75) {
        put_str("rts");
        len = 2;
    } else if (op == 0x4e73) {
        put_str("rte");
        len = 2;
    } else if (op == 0x4e72) {
        rc = peek_u16be(pid, pc + 2u, &imm16);
        if (rc < 0)
            return rc;
        put_str("stop #");
        put_hex16(imm16);
        len = 4;
    } else if ((op & 0xfff0u) == 0x4e40u) {
        put_str("trap #");
        put_u32((uint32_t)(op & 0x000fu));
        len = 2;
    } else if (op == 0x4eb9u || op == 0x4ef9u) {
        rc = peek_u32be(pid, pc + 2u, &imm32);
        if (rc < 0)
            return rc;
        if (op == 0x4eb9u)
            put_str("jsr ");
        else
            put_str("jmp ");
        put_hex32(imm32);
        len = 6;
    } else if ((op & 0xf100u) == 0x7000u) {
        int32_t imm8 = (int32_t)(int8_t)(op & 0x00ffu);
        uint32_t reg = (uint32_t)((op >> 9) & 0x7u);
        put_str("moveq #");
        put_i32(imm8);
        put_str(",d");
        put_u32(reg);
        len = 2;
    } else if ((op & 0xf000u) == 0x6000u) {
        uint32_t target = 0;
        int32_t disp = 0;
        put_str(m68k_cc_name((uint16_t)((op >> 8) & 0x0fu)));
        put_chr(' ');
        disp = (int8_t)(op & 0x00ffu);
        if ((op & 0x00ffu) == 0x00u) {
            rc = peek_u16be(pid, pc + 2u, &imm16);
            if (rc < 0)
                return rc;
            disp = (int16_t)imm16;
            len = 4;
        } else {
            len = 2;
        }
        target = pc + len + (uint32_t)disp;
        put_hex32(target);
    } else {
        put_str("dc.w ");
        put_hex16(op);
        len = 2;
    }

    put_str(" ;");
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = 0;
        rc = peek_u8(pid, pc + i, &b);
        if (rc < 0)
            return rc;
        put_chr(' ');
        put_hex8(b);
    }
    put_chr('\n');
    *next_pc = pc + len;
    return 0;
}

static void disas_m68k(pid_t pid, uint32_t pc, uint32_t count)
{
    if (count == 0)
        count = 8;
    if (count > 64)
        count = 64;

    for (uint32_t i = 0; i < count; i++) {
        int rc = m68k_disas_one(pid, pc, &pc);
        if (rc < 0) {
            put_err("pdb: disas failed rc=");
            put_i32((int32_t)rc);
            put_chr('\n');
            return;
        }
    }
}

static int thumb_disas_one(pid_t pid, uint32_t pc, uint32_t *next_pc)
{
    uint16_t op = 0;
    uint32_t fetch_pc = pc & ~1u;
    uint32_t len = 2;
    int rc = peek_u16le(pid, fetch_pc, &op);
    if (rc < 0)
        return rc;

    put_hex32(fetch_pc);
    put_str(": ");

    if (op == 0xbf00u) {
        put_str("nop");
        len = 2;
    } else if (op == 0x4770u) {
        put_str("bx lr");
        len = 2;
    } else if ((op & 0xf800u) == 0x2000u) {
        uint32_t rd = (op >> 8) & 0x7u;
        uint32_t imm8 = op & 0xffu;
        put_str("movs r");
        put_u32(rd);
        put_str(",#");
        put_u32(imm8);
        len = 2;
    } else if ((op & 0xf800u) == 0x3000u) {
        uint32_t rd = (op >> 8) & 0x7u;
        uint32_t imm8 = op & 0xffu;
        put_str("adds r");
        put_u32(rd);
        put_str(",#");
        put_u32(imm8);
        len = 2;
    } else if ((op & 0xf800u) == 0x3800u) {
        uint32_t rd = (op >> 8) & 0x7u;
        uint32_t imm8 = op & 0xffu;
        put_str("subs r");
        put_u32(rd);
        put_str(",#");
        put_u32(imm8);
        len = 2;
    } else if ((op & 0xf800u) == 0xe000u) {
        int32_t disp = (int32_t)(op & 0x07ffu);
        uint32_t target = 0;
        if (disp & 0x0400u)
            disp |= ~0x07ff;
        disp <<= 1;
        target = fetch_pc + 4u + (uint32_t)disp;
        put_str("b ");
        put_hex32(target);
        len = 2;
    } else if ((op & 0xff00u) == 0xdf00u) {
        put_str("svc #");
        put_u32((uint32_t)(op & 0x00ffu));
        len = 2;
    } else {
        put_str("hword ");
        put_hex16(op);
        len = 2;
    }

    put_str(" ;");
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = 0;
        rc = peek_u8(pid, fetch_pc + i, &b);
        if (rc < 0)
            return rc;
        put_chr(' ');
        put_hex8(b);
    }
    put_chr('\n');
    *next_pc = fetch_pc + len;
    return 0;
}

static void disas_thumb(pid_t pid, uint32_t pc, uint32_t count)
{
    if (count == 0)
        count = 8;
    if (count > 64)
        count = 64;

    for (uint32_t i = 0; i < count; i++) {
        int rc = thumb_disas_one(pid, pc, &pc);
        if (rc < 0) {
            put_err("pdb: disas failed rc=");
            put_i32((int32_t)rc);
            put_chr('\n');
            return;
        }
    }
}

static int z80_is_call_opcode(uint8_t op)
{
    switch (op) {
    case 0xc4:
    case 0xcc:
    case 0xcd:
    case 0xd4:
    case 0xdc:
    case 0xe4:
    case 0xec:
    case 0xf4:
    case 0xfc:
        return 1;
    default:
        return 0;
    }
}

static int wait_child(pid_t pid, int *stopped, int *exit_code,
                      struct ppap_ptrace_event *ev, int report_event)
{
    int status = 0;
    pid_t rc = waitpid(pid, &status, WSTOPPED);

    if (rc != pid) {
        put_err("pdb: waitpid failed\n");
        return -1;
    }

    if (WIFEXITED(status)) {
        *stopped = 0;
        *exit_code = WEXITSTATUS(status);
        if (report_event) {
            put_str("child exited ");
            put_u32((uint32_t)*exit_code);
            put_chr('\n');
        }
        return 1;
    }

    if (!WIFSTOPPED(status)) {
        *stopped = 0;
        put_err("pdb: unexpected wait status\n");
        return -1;
    }

    *stopped = 1;
    if (ptrace(PTRACE_GETEVENT, pid, (void *)0, ev) < 0) {
        put_err("pdb: GETEVENT failed\n");
        return -1;
    }
    if (report_event)
        print_event(ev);
    return 0;
}

static void usage(void)
{
    put_str("Usage: pdb [-q] [--batch] [-c <cmd> ...] [-f <script> ...] <program> [args...]\n");
    put_str("       pdb [-q] [--batch] [-c <cmd> ...] [-f <script> ...] --attach <pid>\n");
}

static void print_help(void)
{
    put_str("options:\n");
    put_str("  -q                suppress prompt/command echo output\n");
    put_str("  --batch           suppress automatic stop/target output\n");
    put_str("  -c <cmd>          queue startup command (repeatable)\n");
    put_str("  -f <script>       load startup commands from file (repeatable)\n");
    put_str("  --attach <pid>    attach to a running process instead of exec\n");
    put_str("                    script format: one command per line, '#' comment\n");
    put_str("\n");
    put_str("commands:\n");
    put_str("  help              show this help\n");
    put_str("  regs              show registers\n");
    put_str("  reg <name|idx>    show one register\n");
    put_str("  caps              show trace capabilities\n");
    put_str("  event             show last stop event\n");
    put_str("  show abi          show current stop ABI\n");
    put_str("  show event        show last stop event\n");
    put_str("  show caps         show trace capabilities\n");
    put_str("  show regset       show current register set\n");
    put_str("  show pc           show current program counter\n");
    put_str("  show sp           show current stack pointer\n");
    put_str("  show surface      show current debug surface\n");
    put_str("  surface <s>       set debug surface (real|ecpu)\n");
    put_str("  where | w         show pc and sp\n");
    put_str("  x <addr> [count]  read memory words\n");
    put_str("  x/<n><fmt> <addr> read memory (<fmt>: x=word, h=half, b=byte)\n");
    put_str("  disas [a] [n]     disassemble n instructions from addr/pc\n");
    put_str("  step | s          single-step\n");
    put_str("  next | n          step over call (z80), else single-step\n");
    put_str("  run | cont | continue | c    continue\n");
    put_str("  set reg <r> <v>   write register by name or index\n");
    put_str("  set mem <a> <v> [size]   write memory (size: b|h|w|1|2|4)\n");
    put_str("  break | b <addr>  set software breakpoint\n");
    put_str("  disable <id>      disable breakpoint by id\n");
    put_str("  enable <id>       enable breakpoint by id\n");
    put_str("  delete | d <id>   clear breakpoint by id\n");
    put_str("  info break|b      show local breakpoint table\n");
    put_str("  detach            detach and quit\n");
    put_str("  quit | q          detach and quit\n");
}

int main(int argc, char *argv[])
{
    int argi = 1;
    int show_prompt = 1;
    int batch_mode = 0;
    int scripted_mode = 0;
    int attach_mode = 0;
    char *script_cmds[PDB_SCRIPT_CMD_MAX];
    int script_storage_used = 0;
    int script_count = 0;
    int script_index = 0;
    pid_t attach_pid = 0;
    pid_t pid;
    int child_stopped = 0;
    int child_exit_code = 0;
    int done = 0;
    char line[PDB_SCRIPT_LINE_MAX];
    char *tok[8];
    struct ppap_ptrace_event last_ev;
    struct ppap_ptrace_caps caps;
    pdb_local_bp_t local_bp[PDB_LOCAL_BP_MAX];

    while (argi < argc) {
        if (streq(argv[argi], "-h") || streq(argv[argi], "--help")) {
            print_help();
            return 0;
        }
        if (streq(argv[argi], "-q")) {
            show_prompt = 0;
            argi++;
            continue;
        }
        if (streq(argv[argi], "--batch")) {
            batch_mode = 1;
            show_prompt = 0;
            argi++;
            continue;
        }
        if (streq(argv[argi], "-c")) {
            int non_space = 0;
            int cmd_len = 0;
            const char *cmd;
            scripted_mode = 1;
            if (argi + 1 >= argc) {
                put_err("pdb: -c requires a command string\n");
                return 1;
            }
            cmd = argv[argi + 1];
            for (cmd_len = 0; cmd[cmd_len]; cmd_len++) {
                if (!is_script_space(cmd[cmd_len])) {
                    non_space = 1;
                    break;
                }
            }
            for (; cmd[cmd_len]; cmd_len++)
                ;
            if (!non_space) {
                argi += 2;
                continue;
            }
            if (cmd_len >= PDB_SCRIPT_LINE_MAX) {
                put_err("pdb: -c command too long\n");
                return 1;
            }
            if (script_count >= PDB_SCRIPT_CMD_MAX) {
                put_err("pdb: too many script commands (max ");
                put_u32(PDB_SCRIPT_CMD_MAX);
                put_str(")\n");
                return 1;
            }
            script_cmds[script_count++] = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (streq(argv[argi], "-f")) {
            scripted_mode = 1;
            if (argi + 1 >= argc) {
                put_err("pdb: -f requires a script path\n");
                return 1;
            }
            if (load_script_file(argv[argi + 1], script_cmds, &script_count,
                                 pdb_script_storage, &script_storage_used) < 0)
                return 1;
            argi += 2;
            continue;
        }
        if (streq(argv[argi], "--attach")) {
            uint32_t parsed_pid = 0;
            if (argi + 1 >= argc) {
                put_err("pdb: --attach requires a pid\n");
                return 1;
            }
            if (!parse_u32(argv[argi + 1], &parsed_pid) ||
                parsed_pid == 0 ||
                parsed_pid > 0x7fffffffu) {
                put_err("pdb: --attach requires a valid positive pid\n");
                return 1;
            }
            attach_mode = 1;
            attach_pid = (pid_t)parsed_pid;
            argi += 2;
            continue;
        }
        break;
    }

    if (!attach_mode && argi >= argc) {
        usage();
        return 1;
    }
    if (attach_mode && argi < argc) {
        put_err("pdb: --attach does not take a program path\n");
        return 1;
    }
    if (scripted_mode && script_count == 0) {
        put_err("pdb: no scripted commands\n");
        return 1;
    }

    for (int i = 0; i < PDB_LOCAL_BP_MAX; i++) {
        local_bp[i].used = 0;
        local_bp[i].enabled = 0;
        local_bp[i].addr = 0;
    }

    if (attach_mode) {
        long rc;
        pid = attach_pid;
        rc = ptrace(PTRACE_ATTACH, pid, (void *)0, (void *)0);
        if (rc < 0) {
            put_err("pdb: ATTACH failed rc=");
            put_i32((int32_t)rc);
            put_chr('\n');
            return 1;
        }
    } else {
        pid = vfork();
        if (pid == 0) {
            ptrace(PTRACE_TRACEME, 0, (void *)0, (void *)0);
            execve(argv[argi], &argv[argi], (void *)0);
            _exit(127);
        }
        if (pid < 0) {
            put_err("pdb: vfork failed\n");
            return 1;
        }
    }

    {
        int wr = wait_child(pid, &child_stopped, &child_exit_code, &last_ev,
                            !batch_mode);
        if (wr < 0)
            return 1;
        if (wr > 0)
            return child_exit_code;
    }

    if (!batch_mode && ptrace(PTRACE_GETCAPS, pid, (void *)0, &caps) == 0) {
        put_str("target ");
        print_caps(&caps);
    }

    while (!done) {
        if (script_index < script_count) {
            int i = 0;
            const char *src = script_cmds[script_index++];
            while (src[i] && i < (int)sizeof(line) - 1) {
                line[i] = src[i];
                i++;
            }
            line[i] = '\0';
            if (show_prompt) {
                put_str("pdb> ");
                put_str(line);
                put_chr('\n');
            }
        } else {
            if (script_count > 0)
                break;
            if (show_prompt)
                put_str("pdb> ");
            if (readline(line, sizeof(line)) < 0) {
                put_chr('\n');
                break;
            }
        }

        int ntok = split_tokens(line, tok, 8);
        if (ntok <= 0)
            continue;

        if (streq(tok[0], "help") || streq(tok[0], "?")) {
            print_help();
            continue;
        }

        if (streq(tok[0], "regs")) {
            struct ppap_ptrace_regs regs;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            if (ptrace(PTRACE_GETREGS, pid, (void *)0, &regs) < 0) {
                put_err("pdb: GETREGS failed\n");
                continue;
            }
            print_regs(&regs);
            continue;
        }

        if (streq(tok[0], "reg")) {
            struct ppap_ptrace_regs regs;
            uint32_t idx = 0;
            int is16 = 0;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            if (ntok < 2) {
                put_err("pdb: usage: reg <name|index>\n");
                continue;
            }
            if (ptrace(PTRACE_GETREGS, pid, (void *)0, &regs) < 0) {
                put_err("pdb: GETREGS failed\n");
                continue;
            }
            if (!reg_index_from_token(&regs, tok[1], &idx)) {
                put_err("pdb: unknown register\n");
                continue;
            }
            is16 = reg_is16(regs.regset);
            {
                const char *name = reg_name(regs.regset, idx);
                if (name) {
                    put_str(name);
                } else {
                    put_str("r");
                    put_u32(idx);
                }
            }
            put_str("=");
            if (is16)
                put_hex16(regs.regs[idx]);
            else
                put_hex32(regs.regs[idx]);
            put_chr('\n');
            continue;
        }

        if (streq(tok[0], "caps")) {
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            if (ptrace(PTRACE_GETCAPS, pid, (void *)0, &caps) < 0) {
                put_err("pdb: GETCAPS failed\n");
                continue;
            }
            print_caps(&caps);
            continue;
        }

        if (streq(tok[0], "event")) {
            print_event(&last_ev);
            continue;
        }

        if (streq(tok[0], "show")) {
            if (ntok < 2) {
                put_err("pdb: usage: show <abi|event|caps|regset|pc|sp|surface>\n");
                continue;
            }
            if (streq(tok[1], "abi")) {
                put_str("abi=");
                put_str(abi_name(last_ev.abi));
                put_chr('\n');
                continue;
            }
            if (streq(tok[1], "event")) {
                print_event(&last_ev);
                continue;
            }
            if (streq(tok[1], "caps")) {
                if (!child_stopped) {
                    put_err("pdb: child is not stopped\n");
                    continue;
                }
                if (ptrace(PTRACE_GETCAPS, pid, (void *)0, &caps) < 0) {
                    put_err("pdb: GETCAPS failed\n");
                    continue;
                }
                print_caps(&caps);
                continue;
            }
            if (streq(tok[1], "regset")) {
                struct ppap_ptrace_regs regs;
                if (!child_stopped) {
                    put_err("pdb: child is not stopped\n");
                    continue;
                }
                if (ptrace(PTRACE_GETREGS, pid, (void *)0, &regs) < 0) {
                    put_err("pdb: GETREGS failed\n");
                    continue;
                }
                put_str("regset=");
                put_str(regset_name(regs.regset));
                put_chr('\n');
                continue;
            }
            if (streq(tok[1], "pc")) {
                struct ppap_ptrace_regs regs;
                uint32_t pc_idx = 0;
                if (!child_stopped) {
                    put_err("pdb: child is not stopped\n");
                    continue;
                }
                if (ptrace(PTRACE_GETREGS, pid, (void *)0, &regs) < 0) {
                    put_err("pdb: GETREGS failed\n");
                    continue;
                }
                switch (regs.regset) {
                case PPAP_TRACE_REGSET_ARM:
                    pc_idx = 15;
                    break;
                case PPAP_TRACE_REGSET_M68K:
                    pc_idx = 16;
                    break;
                case PPAP_TRACE_REGSET_Z80:
                    pc_idx = 7;
                    break;
                default:
                    put_err("pdb: unsupported regset for show pc\n");
                    continue;
                }
                if (pc_idx >= regs.words) {
                    put_err("pdb: pc index out of range\n");
                    continue;
                }
                put_str("pc=");
                if (regs.regset == PPAP_TRACE_REGSET_Z80)
                    put_hex16(regs.regs[pc_idx]);
                else
                    put_hex32(regs.regs[pc_idx]);
                put_chr('\n');
                continue;
            }
            if (streq(tok[1], "sp")) {
                struct ppap_ptrace_regs regs;
                uint32_t sp_idx = 0;
                if (!child_stopped) {
                    put_err("pdb: child is not stopped\n");
                    continue;
                }
                if (ptrace(PTRACE_GETREGS, pid, (void *)0, &regs) < 0) {
                    put_err("pdb: GETREGS failed\n");
                    continue;
                }
                switch (regs.regset) {
                case PPAP_TRACE_REGSET_ARM:
                    sp_idx = 13;
                    break;
                case PPAP_TRACE_REGSET_M68K:
                    sp_idx = 15;
                    break;
                case PPAP_TRACE_REGSET_Z80:
                    sp_idx = 6;
                    break;
                default:
                    put_err("pdb: unsupported regset for show sp\n");
                    continue;
                }
                if (sp_idx >= regs.words) {
                    put_err("pdb: sp index out of range\n");
                    continue;
                }
                put_str("sp=");
                if (regs.regset == PPAP_TRACE_REGSET_Z80)
                    put_hex16(regs.regs[sp_idx]);
                else
                    put_hex32(regs.regs[sp_idx]);
                put_chr('\n');
                continue;
            }
            if (streq(tok[1], "surface")) {
                uint32_t surface = 0;
                if (!child_stopped) {
                    put_err("pdb: child is not stopped\n");
                    continue;
                }
                if (ptrace(PTRACE_GETSURFACE, pid, (void *)0, &surface) == 0) {
                    put_str("surface=");
                    put_str(surface_name_for_value(surface));
                    put_chr('\n');
                    continue;
                }
                /* Fallback for kernels without PTRACE_GETSURFACE support. */
                if (ptrace(PTRACE_GETCAPS, pid, (void *)0, &caps) == 0) {
                    put_str("surface=");
                    put_str(surface_name_for_regset(caps.regset));
                    put_chr('\n');
                    continue;
                }
                put_err("pdb: GETSURFACE/GETCAPS failed\n");
                continue;
            }
            put_err("pdb: usage: show <abi|event|caps|regset|pc|sp|surface>\n");
            continue;
        }

        if (streq(tok[0], "surface")) {
            uint32_t surface = 0;
            long rc;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            if (ntok < 2 || !parse_surface_token(tok[1], &surface)) {
                put_err("pdb: usage: surface <real|ecpu>\n");
                continue;
            }
            rc = ptrace(PTRACE_SETSURFACE, pid, (void *)(uintptr_t)surface, (void *)0);
            if (rc < 0) {
                put_err("pdb: SETSURFACE failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                continue;
            }
            if (ptrace(PTRACE_GETSURFACE, pid, (void *)0, &surface) < 0) {
                put_err("pdb: GETSURFACE failed\n");
                continue;
            }
            put_str("surface=");
            put_str(surface_name_for_value(surface));
            put_chr('\n');
            continue;
        }

        if (streq(tok[0], "where") || streq(tok[0], "w")) {
            struct ppap_ptrace_regs regs;
            uint32_t pc_idx = 0;
            uint32_t sp_idx = 0;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            if (ptrace(PTRACE_GETREGS, pid, (void *)0, &regs) < 0) {
                put_err("pdb: GETREGS failed\n");
                continue;
            }
            switch (regs.regset) {
            case PPAP_TRACE_REGSET_ARM:
                pc_idx = 15;
                sp_idx = 13;
                break;
            case PPAP_TRACE_REGSET_M68K:
                pc_idx = 16;
                sp_idx = 15;
                break;
            case PPAP_TRACE_REGSET_Z80:
                pc_idx = 7;
                sp_idx = 6;
                break;
            default:
                put_err("pdb: unsupported regset for where\n");
                continue;
            }
            if (pc_idx >= regs.words || sp_idx >= regs.words) {
                put_err("pdb: where index out of range\n");
                continue;
            }
            put_str("pc=");
            if (regs.regset == PPAP_TRACE_REGSET_Z80)
                put_hex16(regs.regs[pc_idx]);
            else
                put_hex32(regs.regs[pc_idx]);
            put_str(" sp=");
            if (regs.regset == PPAP_TRACE_REGSET_Z80)
                put_hex16(regs.regs[sp_idx]);
            else
                put_hex32(regs.regs[sp_idx]);
            put_chr('\n');
            continue;
        }

        if (streq(tok[0], "x") || (tok[0][0] == 'x' && tok[0][1] == '/')) {
            uint32_t addr = 0;
            uint32_t count = 4;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            if (streq(tok[0], "x")) {
                if (ntok < 2 || !parse_u32(tok[1], &addr)) {
                    put_err("pdb: usage: x <addr> [count]\n");
                    put_err("pdb:    or: x/<n><fmt> <addr> (fmt: x|h|b)\n");
                    continue;
                }
                if (ntok >= 3 && !parse_u32(tok[2], &count)) {
                    put_err("pdb: invalid count\n");
                    continue;
                }
            } else {
                char fmt = 'x';
                if (!parse_x_spec(tok[0], &count, &fmt) || ntok < 2 ||
                    !parse_u32(tok[1], &addr)) {
                    put_err("pdb: usage: x <addr> [count]\n");
                    put_err("pdb:    or: x/<n><fmt> <addr> (fmt: x|h|b)\n");
                    continue;
                }
                if (count == 0) {
                    put_err("pdb: invalid count\n");
                    continue;
                }
                if (fmt == 'b') {
                    print_mem_bytes(pid, addr, count);
                } else if (fmt == 'h') {
                    print_mem_halfwords(pid, addr, count);
                } else {
                    print_mem_words(pid, addr, count);
                }
                continue;
            }
            print_mem_words(pid, addr, count);
            continue;
        }

        if (streq(tok[0], "disas")) {
            struct ppap_ptrace_regs regs;
            uint32_t addr = 0;
            uint32_t count = 8;
            uint32_t pc_idx = 0;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            if (ptrace(PTRACE_GETREGS, pid, (void *)0, &regs) < 0) {
                put_err("pdb: GETREGS failed\n");
                continue;
            }
            if (regs.regset == PPAP_TRACE_REGSET_Z80) {
                pc_idx = 7; /* Z80 PC */
            } else if (regs.regset == PPAP_TRACE_REGSET_M68K) {
                pc_idx = 16; /* m68k PC */
            } else if (regs.regset == PPAP_TRACE_REGSET_ARM) {
                pc_idx = 15; /* ARM PC */
            } else {
                put_err("pdb: disas currently supports arm, z80, and m68k tracees only\n");
                continue;
            }
            addr = regs.regs[pc_idx];
            if (ntok >= 2 && !parse_u32(tok[1], &addr)) {
                put_err("pdb: usage: disas [addr] [count]\n");
                continue;
            }
            if (ntok >= 3 && !parse_u32(tok[2], &count)) {
                put_err("pdb: invalid count\n");
                continue;
            }
            if (regs.regset == PPAP_TRACE_REGSET_Z80)
                disas_z80(pid, addr, count);
            else if (regs.regset == PPAP_TRACE_REGSET_M68K)
                disas_m68k(pid, addr, count);
            else
                disas_thumb(pid, addr, count);
            continue;
        }

        if (streq(tok[0], "next") || streq(tok[0], "n")) {
            struct ppap_ptrace_regs regs;
            uint8_t op = 0;
            uint32_t pc = 0;
            uint32_t next_pc = 0;
            int use_temp_bp = 0;
            int has_enabled_bp = 0;
            int temp_bp_id = -1;
            long rc;
            int wr;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            if (ptrace(PTRACE_GETREGS, pid, (void *)0, &regs) < 0) {
                put_err("pdb: GETREGS failed\n");
                continue;
            }
            if (regs.regset == PPAP_TRACE_REGSET_Z80) {
                pc = regs.regs[7];  /* Z80 PC */
                rc = (long)peek_u8(pid, pc, &op);
                if (rc < 0) {
                    put_err("pdb: next opcode read failed rc=");
                    put_i32((int32_t)rc);
                    put_chr('\n');
                    continue;
                }

                if (z80_is_call_opcode(op)) {
                    next_pc = pc + 3u;
                    use_temp_bp = 1;
                    for (int i = 0; i < PDB_LOCAL_BP_MAX; i++) {
                        if (!local_bp[i].used || !local_bp[i].enabled)
                            continue;
                        if (local_bp[i].addr == next_pc) {
                            has_enabled_bp = 1;
                            break;
                        }
                    }

                    if (!has_enabled_bp) {
                        struct ppap_ptrace_bp bp;
                        bp.id = -1;
                        bp.addr = next_pc;
                        bp.flags = PPAP_PTRACE_BP_SW;
                        rc = ptrace(PTRACE_SETBP, pid, (void *)0, &bp);
                        if (rc < 0) {
                            put_err("pdb: NEXT SETBP failed rc=");
                            put_i32((int32_t)rc);
                            put_chr('\n');
                            continue;
                        }
                        temp_bp_id = bp.id;
                    }
                }
            }

            if (use_temp_bp) {
                rc = ptrace(PTRACE_CONT, pid, (void *)0, (void *)0);
                if (rc < 0) {
                    put_err("pdb: CONT failed rc=");
                    put_i32((int32_t)rc);
                    put_chr('\n');
                    if (temp_bp_id >= 0) {
                        struct ppap_ptrace_bp bp;
                        bp.id = temp_bp_id;
                        bp.addr = 0;
                        bp.flags = 0;
                        (void)ptrace(PTRACE_CLRBP, pid, (void *)0, &bp);
                    }
                    continue;
                }
                child_stopped = 0;
                wr = wait_child(pid, &child_stopped, &child_exit_code, &last_ev,
                                !batch_mode);
                if (child_stopped && temp_bp_id >= 0) {
                    struct ppap_ptrace_bp bp;
                    bp.id = temp_bp_id;
                    bp.addr = 0;
                    bp.flags = 0;
                    (void)ptrace(PTRACE_CLRBP, pid, (void *)0, &bp);
                }
                if (wr < 0)
                    return 1;
                if (wr > 0)
                    return child_exit_code;
                continue;
            }

            rc = ptrace(PTRACE_SINGLESTEP, pid, (void *)0, (void *)0);
            if (rc < 0) {
                put_err("pdb: SINGLESTEP failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                continue;
            }
            child_stopped = 0;
            wr = wait_child(pid, &child_stopped, &child_exit_code, &last_ev,
                            !batch_mode);
            if (wr < 0)
                return 1;
            if (wr > 0)
                return child_exit_code;
            continue;
        }

        if (streq(tok[0], "step") || streq(tok[0], "s")) {
            long rc;
            int wr;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            rc = ptrace(PTRACE_SINGLESTEP, pid, (void *)0, (void *)0);
            if (rc < 0) {
                put_err("pdb: SINGLESTEP failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                continue;
            }
            child_stopped = 0;
            wr = wait_child(pid, &child_stopped, &child_exit_code, &last_ev,
                            !batch_mode);
            if (wr < 0)
                return 1;
            if (wr > 0)
                return child_exit_code;
            continue;
        }

        if (streq(tok[0], "run") || streq(tok[0], "cont") ||
            streq(tok[0], "continue") || streq(tok[0], "c")) {
            long rc;
            int wr;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            rc = ptrace(PTRACE_CONT, pid, (void *)0, (void *)0);
            if (rc < 0) {
                put_err("pdb: CONT failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                continue;
            }
            child_stopped = 0;
            wr = wait_child(pid, &child_stopped, &child_exit_code, &last_ev,
                            !batch_mode);
            if (wr < 0)
                return 1;
            if (wr > 0)
                return child_exit_code;
            continue;
        }

        if (streq(tok[0], "set")) {
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }

            if (ntok >= 4 && streq(tok[1], "reg")) {
                struct ppap_ptrace_regs regs;
                int is16 = 0;
                uint32_t idx = 0;
                uint32_t value = 0;

                if (!parse_u32(tok[3], &value)) {
                    put_err("pdb: invalid register value\n");
                    continue;
                }
                if (ptrace(PTRACE_GETREGS, pid, (void *)0, &regs) < 0) {
                    put_err("pdb: GETREGS failed\n");
                    continue;
                }
                if (!reg_index_from_token(&regs, tok[2], &idx)) {
                    put_err("pdb: unknown register\n");
                    continue;
                }

                is16 = reg_is16(regs.regset);
                if (is16)
                    value &= 0xffffu;
                regs.regs[idx] = value;
                if (ptrace(PTRACE_SETREGS, pid, (void *)0, &regs) < 0) {
                    put_err("pdb: SETREGS failed\n");
                    continue;
                }

                put_str("reg ");
                {
                    const char *name = reg_name(regs.regset, idx);
                    if (name) {
                        put_str(name);
                    } else {
                        put_str("r");
                        put_u32(idx);
                    }
                }
                put_str("=");
                if (is16)
                    put_hex16(value);
                else
                    put_hex32(value);
                put_chr('\n');
                continue;
            }

            if (ntok >= 4 && streq(tok[1], "mem")) {
                uint32_t addr = 0;
                uint32_t value = 0;
                uint32_t width = 4u;
                long rc;

                if (!parse_u32(tok[2], &addr) || !parse_u32(tok[3], &value) ||
                    (ntok >= 5 && !parse_mem_width(tok[4], &width))) {
                    put_err("pdb: usage: set mem <addr> <value> [size]\n");
                    put_err("pdb:        size: b|h|w (or 1|2|4)\n");
                    continue;
                }
                if (width == 4u) {
                    rc = ptrace(PTRACE_POKEDATA, pid, (void *)(uintptr_t)addr, &value);
                } else {
                    uint32_t base = addr & ~3u;
                    uint32_t old_word = 0;
                    uint32_t new_word = 0;
                    uint32_t shift = (addr & 3u) * 8u;
                    uint32_t mask = (width == 1u) ? 0xffu : 0xffffu;

                    if (width == 2u && (addr & 3u) == 3u) {
                        put_err("pdb: set mem halfword must not cross word boundary\n");
                        continue;
                    }
                    if (width == 2u && (addr & 1u)) {
                        put_err("pdb: set mem halfword requires even address\n");
                        continue;
                    }
                    if ((width == 1u && value > 0xffu) ||
                        (width == 2u && value > 0xffffu)) {
                        put_err("pdb: set mem value out of range for size\n");
                        continue;
                    }

                    rc = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)base, &old_word);
                    if (rc < 0) {
                        put_err("pdb: PEEKDATA failed rc=");
                        put_i32((int32_t)rc);
                        put_chr('\n');
                        continue;
                    }
                    value &= mask;
                    new_word = (old_word & ~(mask << shift)) | (value << shift);
                    rc = ptrace(PTRACE_POKEDATA, pid, (void *)(uintptr_t)base, &new_word);
                }
                if (rc < 0) {
                    put_err("pdb: POKEDATA failed rc=");
                    put_i32((int32_t)rc);
                    put_chr('\n');
                    continue;
                }
                put_str("mem ");
                put_hex32(addr);
                put_str("=");
                if (width == 1u)
                    put_hex8(value);
                else if (width == 2u)
                    put_hex16(value);
                else
                    put_hex32(value);
                put_chr('\n');
                continue;
            }

            put_err("pdb: usage: set reg <name|index> <value>\n");
            put_err("pdb:    or: set mem <addr> <value> [size]\n");
            continue;
        }

        if (streq(tok[0], "break") || streq(tok[0], "b")) {
            struct ppap_ptrace_bp bp;
            long rc;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            if (ntok < 2 || !parse_u32(tok[1], &bp.addr)) {
                put_err("pdb: usage: break <addr>\n");
                continue;
            }
            if (ptrace(PTRACE_GETCAPS, pid, (void *)0, &caps) == 0) {
                if ((caps.caps & PPAP_PTRACE_CAP_SW_BP) == 0u) {
                    if (caps.caps & PPAP_PTRACE_CAP_HW_BP) {
                        put_err("pdb: hardware breakpoints are not yet exposed in pdb\n");
                    } else {
                        put_err("pdb: break not supported on this target/mapping\n");
                    }
                    continue;
                }
            }
            bp.id = -1;
            bp.flags = PPAP_PTRACE_BP_SW;
            rc = ptrace(PTRACE_SETBP, pid, (void *)0, &bp);
            if (rc < 0) {
                put_err("pdb: SETBP failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                continue;
            }
            put_str("bp ");
            put_i32(bp.id);
            put_str(" @ ");
            put_hex32(bp.addr);
            put_chr('\n');
            if (bp.id >= 0 && bp.id < PDB_LOCAL_BP_MAX) {
                local_bp[bp.id].used = 1;
                local_bp[bp.id].enabled = 1;
                local_bp[bp.id].addr = bp.addr;
            }
            continue;
        }

        if (streq(tok[0], "disable")) {
            struct ppap_ptrace_bp bp;
            uint32_t id = 0;
            long rc;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            if (ntok < 2 || !parse_u32(tok[1], &id)) {
                put_err("pdb: usage: disable <id>\n");
                continue;
            }
            if (id >= PDB_LOCAL_BP_MAX || !local_bp[id].used) {
                put_err("pdb: unknown breakpoint id\n");
                continue;
            }
            if (!local_bp[id].enabled) {
                put_str("bp ");
                put_u32(id);
                put_str(" already disabled\n");
                continue;
            }
            bp.id = (int32_t)id;
            bp.addr = 0;
            bp.flags = 0;
            rc = ptrace(PTRACE_CLRBP, pid, (void *)0, &bp);
            if (rc < 0) {
                put_err("pdb: CLRBP failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                continue;
            }
            local_bp[id].enabled = 0;
            put_str("bp ");
            put_u32(id);
            put_str(" disabled\n");
            continue;
        }

        if (streq(tok[0], "enable")) {
            struct ppap_ptrace_bp bp;
            uint32_t id = 0;
            long rc;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            if (ntok < 2 || !parse_u32(tok[1], &id)) {
                put_err("pdb: usage: enable <id>\n");
                continue;
            }
            if (id >= PDB_LOCAL_BP_MAX || !local_bp[id].used) {
                put_err("pdb: unknown breakpoint id\n");
                continue;
            }
            if (local_bp[id].enabled) {
                put_str("bp ");
                put_u32(id);
                put_str(" already enabled\n");
                continue;
            }

            bp.id = -1;
            bp.addr = local_bp[id].addr;
            bp.flags = PPAP_PTRACE_BP_SW;
            rc = ptrace(PTRACE_SETBP, pid, (void *)0, &bp);
            if (rc < 0) {
                put_err("pdb: SETBP failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                continue;
            }
            if (bp.id != (int32_t)id) {
                if (bp.id < 0 || bp.id >= PDB_LOCAL_BP_MAX || local_bp[bp.id].used) {
                    struct ppap_ptrace_bp rollback;
                    rollback.id = bp.id;
                    rollback.addr = 0;
                    rollback.flags = 0;
                    (void)ptrace(PTRACE_CLRBP, pid, (void *)0, &rollback);
                    put_err("pdb: enable remap failed\n");
                    continue;
                }
                local_bp[bp.id].used = local_bp[id].used;
                local_bp[bp.id].enabled = 1;
                local_bp[bp.id].addr = local_bp[id].addr;
                local_bp[id].used = 0;
                local_bp[id].enabled = 0;
                local_bp[id].addr = 0;
                put_str("bp ");
                put_u32(id);
                put_str(" enabled as ");
                put_i32(bp.id);
                put_chr('\n');
                continue;
            }
            local_bp[id].enabled = 1;
            put_str("bp ");
            put_u32(id);
            put_str(" enabled\n");
            continue;
        }

        if (streq(tok[0], "delete") || streq(tok[0], "d")) {
            struct ppap_ptrace_bp bp;
            uint32_t id = 0;
            long rc;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            if (ntok < 2 || !parse_u32(tok[1], &id)) {
                put_err("pdb: usage: delete <id>\n");
                continue;
            }
            if (id >= PDB_LOCAL_BP_MAX || !local_bp[id].used) {
                put_err("pdb: unknown breakpoint id\n");
                continue;
            }
            if (local_bp[id].enabled) {
                bp.id = (int32_t)id;
                bp.addr = 0;
                bp.flags = 0;
                rc = ptrace(PTRACE_CLRBP, pid, (void *)0, &bp);
                if (rc < 0) {
                    put_err("pdb: CLRBP failed rc=");
                    put_i32((int32_t)rc);
                    put_chr('\n');
                    continue;
                }
            }
            put_str("bp ");
            put_u32(id);
            put_str(" cleared\n");
            if (id < PDB_LOCAL_BP_MAX) {
                local_bp[id].used = 0;
                local_bp[id].enabled = 0;
                local_bp[id].addr = 0;
            }
            continue;
        }

        if (streq(tok[0], "info")) {
            if (ntok == 2 && (streq(tok[1], "break") || streq(tok[1], "b"))) {
                int found = 0;
                for (int i = 0; i < PDB_LOCAL_BP_MAX; i++) {
                    if (!local_bp[i].used)
                        continue;
                    found = 1;
                    put_str("bp ");
                    put_u32((uint32_t)i);
                    put_str(" @ ");
                    put_hex32(local_bp[i].addr);
                    put_str(" ");
                    if (local_bp[i].enabled)
                        put_str("enabled");
                    else
                        put_str("disabled");
                    put_chr('\n');
                }
                if (!found)
                    put_str("no breakpoints\n");
                continue;
            }
            put_err("pdb: usage: info break\n");
            continue;
        }

        if (streq(tok[0], "detach")) {
            long rc;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            rc = ptrace(PTRACE_DETACH, pid, (void *)0, (void *)0);
            if (rc < 0) {
                put_err("pdb: DETACH failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                continue;
            }
            child_stopped = 0;
            put_str("detached\n");
            done = 1;
            continue;
        }

        if (streq(tok[0], "quit") || streq(tok[0], "q")) {
            if (child_stopped) {
                long rc = ptrace(PTRACE_DETACH, pid, (void *)0, (void *)0);
                if (rc < 0) {
                    put_err("pdb: DETACH failed rc=");
                    put_i32((int32_t)rc);
                    put_chr('\n');
                    continue;
                }
                child_stopped = 0;
                put_str("detached\n");
            }
            done = 1;
            continue;
        }

        put_err("pdb: unknown command\n");
    }

    return 0;
}
