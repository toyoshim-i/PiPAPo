#include "syscall.h"

#define PDB_LOCAL_BP_MAX  32

typedef struct {
    uint8_t used;
    uint32_t addr;
} pdb_local_bp_t;

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

static void print_event(const struct ppap_ptrace_event *ev)
{
    put_str("stop ");
    put_str(event_name(ev->event));
    put_str(" abi=");
    put_str(abi_name(ev->abi));

    if (ev->event == PPAP_TRACE_EVENT_DEBUG_STOP) {
        put_str(" pc=");
        put_hex32(ev->args[0]);
        put_str(" flags=");
        put_hex32(ev->flags);
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
    put_str(" caps=");
    put_hex32(caps->caps);
    put_str(" max_bps=");
    put_u32(caps->max_bps);
    put_chr('\n');
}

static void print_regs(const struct ppap_ptrace_regs *regs)
{
    static const char *arm_names[] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc", "xpsr"
    };
    static const char *m68k_names[] = {
        "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
        "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
        "pc", "sr", "usp", "ksp"
    };
    static const char *z80_names[] = {
        "af", "bc", "de", "hl", "ix", "iy", "sp", "pc",
        "af2", "bc2", "de2", "hl2", "iff1", "iff2", "im", "wz",
        "i", "r"
    };

    const char **names = (const char **)0;
    uint32_t name_count = 0;

    if (regs->regset == PPAP_TRACE_REGSET_ARM) {
        names = arm_names;
        name_count = 17;
    } else if (regs->regset == PPAP_TRACE_REGSET_M68K) {
        names = m68k_names;
        name_count = 20;
    } else if (regs->regset == PPAP_TRACE_REGSET_Z80) {
        names = z80_names;
        name_count = 18;
    }

    put_str("regset=");
    put_str(regset_name(regs->regset));
    put_str(" abi=");
    put_str(abi_name(regs->abi));
    put_str(" words=");
    put_u32(regs->words);
    put_chr('\n');

    for (uint32_t i = 0; i < regs->words && i < PPAP_PTRACE_REGS_MAX; i++) {
        if (names && i < name_count) {
            put_str(names[i]);
        } else {
            put_str("r");
            put_u32(i);
        }
        put_str("=");
        if (regs->regset == PPAP_TRACE_REGSET_Z80)
            put_hex16(regs->regs[i]);
        else
            put_hex32(regs->regs[i]);
        put_chr('\n');
    }
}

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

static int wait_child(pid_t pid, int *stopped, int *exit_code,
                      struct ppap_ptrace_event *ev)
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
        put_str("child exited ");
        put_u32((uint32_t)*exit_code);
        put_chr('\n');
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
    print_event(ev);
    return 0;
}

static void usage(void)
{
    put_str("Usage: pdb <program> [args...]\n");
}

static void print_help(void)
{
    put_str("commands:\n");
    put_str("  help              show this help\n");
    put_str("  regs              show registers\n");
    put_str("  caps              show trace capabilities\n");
    put_str("  event             show last stop event\n");
    put_str("  x <addr> [count]  read memory words\n");
    put_str("  step | s          single-step\n");
    put_str("  cont | c          continue\n");
    put_str("  break <addr>      set software breakpoint\n");
    put_str("  delete <id>       clear breakpoint by id\n");
    put_str("  info break        show local breakpoint table\n");
    put_str("  quit | q          detach and quit\n");
}

int main(int argc, char *argv[])
{
    pid_t pid;
    int child_stopped = 0;
    int child_exit_code = 0;
    int done = 0;
    char line[128];
    char *tok[8];
    struct ppap_ptrace_event last_ev;
    struct ppap_ptrace_caps caps;
    pdb_local_bp_t local_bp[PDB_LOCAL_BP_MAX];

    if (argc < 2) {
        usage();
        return 1;
    }

    for (int i = 0; i < PDB_LOCAL_BP_MAX; i++) {
        local_bp[i].used = 0;
        local_bp[i].addr = 0;
    }

    pid = vfork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, (void *)0, (void *)0);
        execve(argv[1], &argv[1], (void *)0);
        _exit(127);
    }
    if (pid < 0) {
        put_err("pdb: vfork failed\n");
        return 1;
    }

    {
        int wr = wait_child(pid, &child_stopped, &child_exit_code, &last_ev);
        if (wr < 0)
            return 1;
        if (wr > 0)
            return child_exit_code;
    }

    if (ptrace(PTRACE_GETCAPS, pid, (void *)0, &caps) == 0) {
        put_str("target ");
        print_caps(&caps);
    }

    while (!done) {
        put_str("pdb> ");
        if (readline(line, sizeof(line)) < 0) {
            put_chr('\n');
            break;
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

        if (streq(tok[0], "x")) {
            uint32_t addr = 0;
            uint32_t count = 4;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                continue;
            }
            if (ntok < 2 || !parse_u32(tok[1], &addr)) {
                put_err("pdb: usage: x <addr> [count]\n");
                continue;
            }
            if (ntok >= 3 && !parse_u32(tok[2], &count)) {
                put_err("pdb: invalid count\n");
                continue;
            }
            print_mem_words(pid, addr, count);
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
            wr = wait_child(pid, &child_stopped, &child_exit_code, &last_ev);
            if (wr < 0)
                return 1;
            if (wr > 0)
                return child_exit_code;
            continue;
        }

        if (streq(tok[0], "cont") || streq(tok[0], "c")) {
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
            wr = wait_child(pid, &child_stopped, &child_exit_code, &last_ev);
            if (wr < 0)
                return 1;
            if (wr > 0)
                return child_exit_code;
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
                local_bp[bp.id].addr = bp.addr;
            }
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
            put_str("bp ");
            put_u32(id);
            put_str(" cleared\n");
            if (id < PDB_LOCAL_BP_MAX) {
                local_bp[id].used = 0;
                local_bp[id].addr = 0;
            }
            continue;
        }

        if (streq(tok[0], "info") && ntok >= 2 && streq(tok[1], "break")) {
            for (int i = 0; i < PDB_LOCAL_BP_MAX; i++) {
                if (!local_bp[i].used)
                    continue;
                put_str("bp ");
                put_u32((uint32_t)i);
                put_str(" @ ");
                put_hex32(local_bp[i].addr);
                put_chr('\n');
            }
            continue;
        }

        if (streq(tok[0], "quit") || streq(tok[0], "q")) {
            if (child_stopped)
                (void)ptrace(PTRACE_DETACH, pid, (void *)0, (void *)0);
            done = 1;
            continue;
        }

        put_err("pdb: unknown command\n");
    }

    return 0;
}
