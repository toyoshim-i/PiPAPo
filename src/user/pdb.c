#include "syscall.h"
#include "pdb_trace_util.h"
#include "pdb_util.h"

#define PDB_LOCAL_BP_MAX  32
#define PDB_SCRIPT_CMD_MAX  32
#define PDB_SCRIPT_LINE_MAX  128
#define PDB_SCRIPT_BUF_MAX  2048

typedef struct {
    uint8_t used;
    uint8_t enabled;
    uint32_t addr;
    uint32_t flags;
} pdb_local_bp_t;

static char pdb_script_storage[PDB_SCRIPT_BUF_MAX];

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

static int regset_pc_sp_indices(uint32_t regset, uint32_t *pc_idx, uint32_t *sp_idx)
{
    switch (regset) {
    case PPAP_TRACE_REGSET_ARM:
        *pc_idx = 15u;
        *sp_idx = 13u;
        return 1;
    case PPAP_TRACE_REGSET_M68K:
        *pc_idx = 16u;
        *sp_idx = 15u;
        return 1;
    case PPAP_TRACE_REGSET_Z80:
        *pc_idx = 7u;
        *sp_idx = 6u;
        return 1;
    default:
        return 0;
    }
}

static int regset_pc_index(uint32_t regset, uint32_t *pc_idx)
{
    uint32_t sp_idx = 0;
    return regset_pc_sp_indices(regset, pc_idx, &sp_idx);
}

static void print_reg_value(uint32_t regset, uint32_t value)
{
    if (reg_is16(regset))
        put_hex16(value);
    else
        put_hex32(value);
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

static int ensure_child_stopped(int child_stopped)
{
    if (child_stopped)
        return 1;
    put_err("pdb: child is not stopped\n");
    return 0;
}

static int get_regs_if_stopped(pid_t pid, int child_stopped,
                               struct ppap_ptrace_regs *regs)
{
    if (!ensure_child_stopped(child_stopped))
        return 0;
    if (ptrace(PTRACE_GETREGS, pid, (void *)0, regs) < 0) {
        put_err("pdb: GETREGS failed\n");
        return 0;
    }
    return 1;
}

static int get_caps_if_stopped(pid_t pid, int child_stopped,
                               struct ppap_ptrace_caps *caps)
{
    if (!ensure_child_stopped(child_stopped))
        return 0;
    if (ptrace(PTRACE_GETCAPS, pid, (void *)0, caps) < 0) {
        put_err("pdb: GETCAPS failed\n");
        return 0;
    }
    return 1;
}

static void init_local_bp_table(pdb_local_bp_t *local_bp)
{
    for (int i = 0; i < PDB_LOCAL_BP_MAX; i++) {
        local_bp[i].used = 0;
        local_bp[i].enabled = 0;
        local_bp[i].addr = 0;
        local_bp[i].flags = 0;
    }
}

static int start_tracee(char *argv[], int argi, int attach_mode, pid_t attach_pid,
                        pid_t *pid_out)
{
    pid_t pid = 0;

    if (attach_mode) {
        long rc;
        pid = attach_pid;
        rc = ptrace(PTRACE_ATTACH, pid, (void *)0, (void *)0);
        if (rc < 0) {
            put_err("pdb: ATTACH failed rc=");
            put_i32((int32_t)rc);
            put_chr('\n');
            return 0;
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
            return 0;
        }
    }

    *pid_out = pid;
    return 1;
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
    put_str("  pc                show current program counter\n");
    put_str("  sp                show current stack pointer\n");
    put_str("  show surface      show current debug surface\n");
    put_str("  surface <s>       set debug surface (real|ecpu)\n");
    put_str("  where | w         show pc and sp\n");
    put_str("  bt [count]        show a simple frame-pointer backtrace\n");
    put_str("  x <addr> [count]  read memory words\n");
    put_str("  x/<n><fmt> <addr> read memory (<fmt>: x=word, h=half, b=byte)\n");
    put_str("  mem <a> [n] [sz]  read memory with size b|h|w|1|2|4\n");
    put_str("  disas [a] [n]     disassemble n instructions from addr/pc\n");
    put_str("  step | s          single-step\n");
    put_str("  next | n          step over call (z80), else single-step\n");
    put_str("  run | cont | continue | c    continue\n");
    put_str("  set reg <r> <v>   write register by name or index\n");
    put_str("  set mem <a> <v> [size]   write memory (size: b|h|w|1|2|4)\n");
    put_str("  restore mem <a> <bytes...>  write byte sequence\n");
    put_str("  break | b <addr>  set breakpoint (auto sw/hw from caps)\n");
    put_str("  break | b <sw|hw> <addr>  force breakpoint type\n");
    put_str("  disable <id>      disable breakpoint by id\n");
    put_str("  enable <id>       enable breakpoint by id\n");
    put_str("  delete | d <id>   clear breakpoint by id\n");
    put_str("  info break|b      show local breakpoint table\n");
    put_str("  detach            detach and quit\n");
    put_str("  quit | q          detach and quit\n");
}

static int parse_startup_options(int argc, char *argv[],
                                 int *argi, int *show_prompt, int *batch_mode,
                                 int *scripted_mode, int *attach_mode,
                                 pid_t *attach_pid, char **script_cmds,
                                 int *script_count, char *script_storage,
                                 int *script_storage_used, int *exit_code)
{
    while (*argi < argc) {
        if (streq(argv[*argi], "-h") || streq(argv[*argi], "--help")) {
            print_help();
            *exit_code = 0;
            return 0;
        }
        if (streq(argv[*argi], "-q")) {
            *show_prompt = 0;
            (*argi)++;
            continue;
        }
        if (streq(argv[*argi], "--batch")) {
            *batch_mode = 1;
            *show_prompt = 0;
            (*argi)++;
            continue;
        }
        if (streq(argv[*argi], "-c")) {
            int non_space = 0;
            int cmd_len = 0;
            const char *cmd;
            *scripted_mode = 1;
            if (*argi + 1 >= argc) {
                put_err("pdb: -c requires a command string\n");
                *exit_code = 1;
                return 0;
            }
            cmd = argv[*argi + 1];
            for (cmd_len = 0; cmd[cmd_len]; cmd_len++) {
                if (!is_script_space(cmd[cmd_len])) {
                    non_space = 1;
                    break;
                }
            }
            for (; cmd[cmd_len]; cmd_len++)
                ;
            if (!non_space) {
                *argi += 2;
                continue;
            }
            if (cmd_len >= PDB_SCRIPT_LINE_MAX) {
                put_err("pdb: -c command too long\n");
                *exit_code = 1;
                return 0;
            }
            if (*script_count >= PDB_SCRIPT_CMD_MAX) {
                put_err("pdb: too many script commands (max ");
                put_u32(PDB_SCRIPT_CMD_MAX);
                put_str(")\n");
                *exit_code = 1;
                return 0;
            }
            script_cmds[(*script_count)++] = argv[*argi + 1];
            *argi += 2;
            continue;
        }
        if (streq(argv[*argi], "-f")) {
            *scripted_mode = 1;
            if (*argi + 1 >= argc) {
                put_err("pdb: -f requires a script path\n");
                *exit_code = 1;
                return 0;
            }
            if (load_script_file(argv[*argi + 1], script_cmds, script_count,
                                 script_storage, script_storage_used) < 0) {
                *exit_code = 1;
                return 0;
            }
            *argi += 2;
            continue;
        }
        if (streq(argv[*argi], "--attach")) {
            uint32_t parsed_pid = 0;
            if (*argi + 1 >= argc) {
                put_err("pdb: --attach requires a pid\n");
                *exit_code = 1;
                return 0;
            }
            if (!parse_u32(argv[*argi + 1], &parsed_pid) ||
                parsed_pid == 0 ||
                parsed_pid > 0x7fffffffu) {
                put_err("pdb: --attach requires a valid positive pid\n");
                *exit_code = 1;
                return 0;
            }
            *attach_mode = 1;
            *attach_pid = (pid_t)parsed_pid;
            *argi += 2;
            continue;
        }
        break;
    }
    return 1;
}

static int validate_startup_options(int argc, int argi,
                                    int attach_mode, int scripted_mode,
                                    int script_count)
{
    if (!attach_mode && argi >= argc) {
        usage();
        return 0;
    }
    if (attach_mode && argi < argc) {
        put_err("pdb: --attach does not take a program path\n");
        return 0;
    }
    if (scripted_mode && script_count == 0) {
        put_err("pdb: no scripted commands\n");
        return 0;
    }
    return 1;
}

static int read_next_command_line(char *line, int line_size,
                                  char **script_cmds, int script_count,
                                  int *script_index, int show_prompt)
{
    if (*script_index < script_count) {
        int i = 0;
        const char *src = script_cmds[(*script_index)++];
        while (src[i] && i < line_size - 1) {
            line[i] = src[i];
            i++;
        }
        line[i] = '\0';
        if (show_prompt) {
            put_str("pdb> ");
            put_str(line);
            put_chr('\n');
        }
        return 1;
    }

    if (script_count > 0)
        return 0;
    if (show_prompt)
        put_str("pdb> ");
    if (readline(line, line_size) < 0) {
        put_chr('\n');
        return 0;
    }
    return 1;
}

static int handle_inspect_commands(pid_t pid, int child_stopped,
                                   char **tok, int ntok,
                                   struct ppap_ptrace_event *last_ev,
                                   struct ppap_ptrace_caps *caps)
{
    if (streq(tok[0], "help") || streq(tok[0], "?")) {
        if (ntok != 1) {
            put_err("pdb: usage: help\n");
            return 1;
        }
        print_help();
        return 1;
    }

    if (streq(tok[0], "regs")) {
        struct ppap_ptrace_regs regs;
        if (ntok != 1) {
            put_err("pdb: usage: regs\n");
            return 1;
        }
        if (!get_regs_if_stopped(pid, child_stopped, &regs))
            return 1;
        print_regs(&regs);
        return 1;
    }

    if (streq(tok[0], "reg")) {
        struct ppap_ptrace_regs regs;
        uint32_t idx = 0;
        int is16 = 0;
        if (ntok != 2) {
            put_err("pdb: usage: reg <name|index>\n");
            return 1;
        }
        if (!get_regs_if_stopped(pid, child_stopped, &regs))
            return 1;
        if (!reg_index_from_token(&regs, tok[1], &idx)) {
            put_err("pdb: unknown register\n");
            return 1;
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
        return 1;
    }

    if (streq(tok[0], "caps")) {
        if (ntok != 1) {
            put_err("pdb: usage: caps\n");
            return 1;
        }
        if (!get_caps_if_stopped(pid, child_stopped, caps))
            return 1;
        print_caps(caps);
        return 1;
    }

    if (streq(tok[0], "event")) {
        if (ntok != 1) {
            put_err("pdb: usage: event\n");
            return 1;
        }
        print_event(last_ev);
        return 1;
    }

    if (streq(tok[0], "show") || streq(tok[0], "pc") || streq(tok[0], "sp")) {
        const char *show_item = 0;
        if (streq(tok[0], "show")) {
            if (ntok != 2) {
                put_err("pdb: usage: show <abi|event|caps|regset|pc|sp|surface>\n");
                return 1;
            }
            show_item = tok[1];
        } else {
            if (ntok != 1) {
                if (streq(tok[0], "pc"))
                    put_err("pdb: usage: pc\n");
                else
                    put_err("pdb: usage: sp\n");
                return 1;
            }
            show_item = tok[0];
        }
        if (streq(show_item, "abi")) {
            put_str("abi=");
            put_str(abi_name(last_ev->abi));
            put_chr('\n');
            return 1;
        }
        if (streq(show_item, "event")) {
            print_event(last_ev);
            return 1;
        }
        if (streq(show_item, "caps")) {
            if (!get_caps_if_stopped(pid, child_stopped, caps))
                return 1;
            print_caps(caps);
            return 1;
        }
        if (streq(show_item, "regset")) {
            struct ppap_ptrace_regs regs;
            if (!get_regs_if_stopped(pid, child_stopped, &regs))
                return 1;
            put_str("regset=");
            put_str(regset_name(regs.regset));
            put_chr('\n');
            return 1;
        }
        if (streq(show_item, "pc")) {
            struct ppap_ptrace_regs regs;
            uint32_t pc_idx = 0;
            uint32_t sp_idx = 0;
            if (!get_regs_if_stopped(pid, child_stopped, &regs))
                return 1;
            if (!regset_pc_sp_indices(regs.regset, &pc_idx, &sp_idx)) {
                put_err("pdb: unsupported regset for show pc\n");
                return 1;
            }
            if (pc_idx >= regs.words) {
                put_err("pdb: pc index out of range\n");
                return 1;
            }
            put_str("pc=");
            print_reg_value(regs.regset, regs.regs[pc_idx]);
            put_chr('\n');
            return 1;
        }
        if (streq(show_item, "sp")) {
            struct ppap_ptrace_regs regs;
            uint32_t pc_idx = 0;
            uint32_t sp_idx = 0;
            if (!get_regs_if_stopped(pid, child_stopped, &regs))
                return 1;
            if (!regset_pc_sp_indices(regs.regset, &pc_idx, &sp_idx)) {
                put_err("pdb: unsupported regset for show sp\n");
                return 1;
            }
            if (sp_idx >= regs.words) {
                put_err("pdb: sp index out of range\n");
                return 1;
            }
            put_str("sp=");
            print_reg_value(regs.regset, regs.regs[sp_idx]);
            put_chr('\n');
            return 1;
        }
        if (streq(show_item, "surface")) {
            uint32_t surface = 0;
            if (!child_stopped) {
                put_err("pdb: child is not stopped\n");
                return 1;
            }
            if (ptrace(PTRACE_GETSURFACE, pid, (void *)0, &surface) == 0) {
                put_str("surface=");
                put_str(surface_name_for_value(surface));
                put_chr('\n');
                return 1;
            }
            /* Fallback for kernels without PTRACE_GETSURFACE support. */
            if (ptrace(PTRACE_GETCAPS, pid, (void *)0, caps) == 0) {
                put_str("surface=");
                put_str(surface_name_for_regset(caps->regset));
                put_chr('\n');
                return 1;
            }
            put_err("pdb: GETSURFACE/GETCAPS failed\n");
            return 1;
        }
        if (streq(tok[0], "show"))
            put_err("pdb: usage: show <abi|event|caps|regset|pc|sp|surface>\n");
        else if (streq(tok[0], "pc"))
            put_err("pdb: usage: pc\n");
        else
            put_err("pdb: usage: sp\n");
        return 1;
    }

    if (streq(tok[0], "surface")) {
        uint32_t surface = 0;
        long rc;
        if (!child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        if (ntok != 2 || !parse_surface_token(tok[1], &surface)) {
            put_err("pdb: usage: surface <real|ecpu>\n");
            return 1;
        }
        rc = ptrace(PTRACE_SETSURFACE, pid, (void *)(uintptr_t)surface, (void *)0);
        if (rc < 0) {
            put_err("pdb: SETSURFACE failed rc=");
            put_i32((int32_t)rc);
            put_err(" (target may not support requested surface)\n");
            return 1;
        }
        if (ptrace(PTRACE_GETSURFACE, pid, (void *)0, &surface) < 0) {
            put_err("pdb: GETSURFACE failed\n");
            return 1;
        }
        put_str("surface=");
        put_str(surface_name_for_value(surface));
        put_chr('\n');
        return 1;
    }

    if (streq(tok[0], "where") || streq(tok[0], "w")) {
        struct ppap_ptrace_regs regs;
        uint32_t pc_idx = 0;
        uint32_t sp_idx = 0;
        if (ntok != 1) {
            put_err("pdb: usage: where\n");
            return 1;
        }
        if (!get_regs_if_stopped(pid, child_stopped, &regs))
            return 1;
        if (!regset_pc_sp_indices(regs.regset, &pc_idx, &sp_idx)) {
            put_err("pdb: unsupported regset for where\n");
            return 1;
        }
        if (pc_idx >= regs.words || sp_idx >= regs.words) {
            put_err("pdb: where index out of range\n");
            return 1;
        }
        put_str("pc=");
        print_reg_value(regs.regset, regs.regs[pc_idx]);
        put_str(" sp=");
        print_reg_value(regs.regset, regs.regs[sp_idx]);
        put_chr('\n');
        return 1;
    }

    if (streq(tok[0], "bt")) {
        struct ppap_ptrace_regs regs;
        uint32_t count = 8;
        if (ntok > 2) {
            put_err("pdb: usage: bt [count]\n");
            return 1;
        }
        if (ntok == 2 && !parse_u32(tok[1], &count)) {
            put_err("pdb: usage: bt [count]\n");
            return 1;
        }
        if (!get_regs_if_stopped(pid, child_stopped, &regs))
            return 1;
        print_backtrace(pid, &regs, count);
        return 1;
    }

    if (streq(tok[0], "mem")) {
        uint32_t addr = 0;
        uint32_t count = 4;
        uint32_t width = 4;
        if (!child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        if (ntok < 2 || ntok > 4 || !parse_u32(tok[1], &addr)) {
            put_err("pdb: usage: mem <addr> [count] [size]\n");
            put_err("pdb:        size: b|h|w (or 1|2|4)\n");
            return 1;
        }
        if (ntok >= 3 && !parse_u32(tok[2], &count)) {
            put_err("pdb: invalid count\n");
            return 1;
        }
        if (ntok == 4 && !parse_mem_width(tok[3], &width)) {
            put_err("pdb: usage: mem <addr> [count] [size]\n");
            put_err("pdb:        size: b|h|w (or 1|2|4)\n");
            return 1;
        }
        if (count == 0) {
            put_err("pdb: invalid count\n");
            return 1;
        }
        if (width == 1u)
            print_mem_bytes(pid, addr, count);
        else if (width == 2u)
            print_mem_halfwords(pid, addr, count);
        else
            print_mem_words(pid, addr, count);
        return 1;
    }

    if (streq(tok[0], "x") || (tok[0][0] == 'x' && tok[0][1] == '/')) {
        uint32_t addr = 0;
        uint32_t count = 4;
        if (!child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        if (streq(tok[0], "x")) {
            if (ntok < 2 || ntok > 3 || !parse_u32(tok[1], &addr)) {
                put_err("pdb: usage: x <addr> [count]\n");
                put_err("pdb:    or: x/<n><fmt> <addr> (fmt: x|h|b)\n");
                return 1;
            }
            if (ntok == 3 && !parse_u32(tok[2], &count)) {
                put_err("pdb: invalid count\n");
                return 1;
            }
        } else {
            char fmt = 'x';
            if (!parse_x_spec(tok[0], &count, &fmt) || ntok != 2 ||
                !parse_u32(tok[1], &addr)) {
                put_err("pdb: usage: x <addr> [count]\n");
                put_err("pdb:    or: x/<n><fmt> <addr> (fmt: x|h|b)\n");
                return 1;
            }
            if (count == 0) {
                put_err("pdb: invalid count\n");
                return 1;
            }
            if (fmt == 'b') {
                print_mem_bytes(pid, addr, count);
            } else if (fmt == 'h') {
                print_mem_halfwords(pid, addr, count);
            } else {
                print_mem_words(pid, addr, count);
            }
            return 1;
        }
        print_mem_words(pid, addr, count);
        return 1;
    }

    if (streq(tok[0], "disas")) {
        struct ppap_ptrace_regs regs;
        uint32_t addr = 0;
        uint32_t count = 8;
        uint32_t pc_idx = 0;
        if (!get_regs_if_stopped(pid, child_stopped, &regs))
            return 1;
        if (!regset_pc_index(regs.regset, &pc_idx)) {
            put_err("pdb: disas currently supports arm, z80, and m68k tracees only\n");
            return 1;
        }
        addr = regs.regs[pc_idx];
        if (ntok > 3) {
            put_err("pdb: usage: disas [addr] [count]\n");
            return 1;
        }
        if (ntok >= 2 && !parse_u32(tok[1], &addr)) {
            put_err("pdb: usage: disas [addr] [count]\n");
            return 1;
        }
        if (ntok >= 3 && !parse_u32(tok[2], &count)) {
            put_err("pdb: invalid count\n");
            return 1;
        }
        if (regs.regset == PPAP_TRACE_REGSET_Z80)
            disas_z80(pid, addr, count);
        else if (regs.regset == PPAP_TRACE_REGSET_M68K)
            disas_m68k(pid, addr, count);
        else
            disas_thumb(pid, addr, count);
        return 1;
    }

    return 0;
}

/* Returns 1 when command completed, 2 when caller should return main_exit_code. */
static int resume_and_wait(pid_t pid, int request, const char *request_name,
                           int *child_stopped, int *child_exit_code,
                           struct ppap_ptrace_event *last_ev, int batch_mode,
                           int *main_exit_code)
{
    long rc = ptrace(request, pid, (void *)0, (void *)0);
    int wr;
    if (rc < 0) {
        put_err("pdb: ");
        put_err(request_name);
        put_err(" failed rc=");
        put_i32((int32_t)rc);
        put_chr('\n');
        return 1;
    }
    *child_stopped = 0;
    wr = wait_child(pid, child_stopped, child_exit_code, last_ev, !batch_mode);
    if (wr < 0) {
        *main_exit_code = 1;
        return 2;
    }
    if (wr > 0) {
        *main_exit_code = *child_exit_code;
        return 2;
    }
    return 1;
}

/* Returns: 0=not handled, 1=handled, 2=caller should return main_exit_code. */
static int handle_run_control_commands(pid_t pid, int *child_stopped,
                                       int *child_exit_code, char **tok,
                                       int ntok,
                                       struct ppap_ptrace_event *last_ev,
                                       struct ppap_ptrace_caps *caps,
                                       pdb_local_bp_t *local_bp,
                                       int batch_mode, int *main_exit_code)
{
    if (streq(tok[0], "next") || streq(tok[0], "n")) {
        struct ppap_ptrace_regs regs;
        uint8_t op = 0;
        uint32_t pc = 0;
        uint32_t next_pc = 0;
        uint32_t next_bp_flag = PPAP_PTRACE_BP_SW;
        int use_temp_bp = 0;
        int has_enabled_bp = 0;
        int temp_bp_id = -1;
        long rc;
        if (ntok != 1) {
            put_err("pdb: usage: next\n");
            return 1;
        }
        if (!get_regs_if_stopped(pid, *child_stopped, &regs))
            return 1;
        if (ptrace(PTRACE_GETCAPS, pid, (void *)0, caps) == 0) {
            uint32_t cap_flag = select_bp_flag_from_caps(caps->caps);
            if (cap_flag == 0) {
                put_err("pdb: break not supported on this target/mapping\n");
                return 1;
            }
            next_bp_flag = cap_flag;
        }
        if (regs.regset == PPAP_TRACE_REGSET_Z80) {
            pc = regs.regs[7];  /* Z80 PC */
            rc = (long)peek_u8(pid, pc, &op);
            if (rc < 0) {
                put_err("pdb: next opcode read failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                return 1;
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
                    bp.flags = next_bp_flag;
                    rc = ptrace(PTRACE_SETBP, pid, (void *)0, &bp);
                    if (rc < 0) {
                        put_err("pdb: NEXT SETBP failed rc=");
                        put_i32((int32_t)rc);
                        put_chr('\n');
                        return 1;
                    }
                    temp_bp_id = bp.id;
                }
            }
        }

        if (use_temp_bp) {
            int rr = resume_and_wait(pid, PTRACE_CONT, "CONT", child_stopped,
                                     child_exit_code, last_ev, batch_mode,
                                     main_exit_code);
            if (*child_stopped && temp_bp_id >= 0) {
                struct ppap_ptrace_bp bp;
                bp.id = temp_bp_id;
                bp.addr = 0;
                bp.flags = 0;
                (void)ptrace(PTRACE_CLRBP, pid, (void *)0, &bp);
            }
            return rr;
        }
        return resume_and_wait(pid, PTRACE_SINGLESTEP, "SINGLESTEP",
                               child_stopped, child_exit_code, last_ev,
                               batch_mode, main_exit_code);
    }

    if (streq(tok[0], "step") || streq(tok[0], "s")) {
        if (ntok != 1) {
            put_err("pdb: usage: step\n");
            return 1;
        }
        if (!*child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        return resume_and_wait(pid, PTRACE_SINGLESTEP, "SINGLESTEP",
                               child_stopped, child_exit_code, last_ev,
                               batch_mode, main_exit_code);
    }

    if (streq(tok[0], "run") || streq(tok[0], "cont") ||
        streq(tok[0], "continue") || streq(tok[0], "c")) {
        if (ntok != 1) {
            put_err("pdb: usage: cont\n");
            return 1;
        }
        if (!*child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        return resume_and_wait(pid, PTRACE_CONT, "CONT", child_stopped,
                               child_exit_code, last_ev, batch_mode,
                               main_exit_code);
    }

    return 0;
}

static int handle_write_commands(pid_t pid, int child_stopped,
                                 char **tok, int ntok)
{
    if (streq(tok[0], "set")) {
        if (!child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }

        if (ntok == 4 && streq(tok[1], "reg")) {
            struct ppap_ptrace_regs regs;
            int is16 = 0;
            uint32_t idx = 0;
            uint32_t value = 0;

            if (!parse_u32(tok[3], &value)) {
                put_err("pdb: invalid register value\n");
                return 1;
            }
            if (ptrace(PTRACE_GETREGS, pid, (void *)0, &regs) < 0) {
                put_err("pdb: GETREGS failed\n");
                return 1;
            }
            if (!reg_index_from_token(&regs, tok[2], &idx)) {
                put_err("pdb: unknown register\n");
                return 1;
            }

            is16 = reg_is16(regs.regset);
            if (is16)
                value &= 0xffffu;
            regs.regs[idx] = value;
            if (ptrace(PTRACE_SETREGS, pid, (void *)0, &regs) < 0) {
                put_err("pdb: SETREGS failed\n");
                return 1;
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
            return 1;
        }

        if ((ntok == 4 || ntok == 5) && streq(tok[1], "mem")) {
            uint32_t addr = 0;
            uint32_t value = 0;
            uint32_t width = 4u;
            long rc;

            if (!parse_u32(tok[2], &addr) || !parse_u32(tok[3], &value) ||
                (ntok == 5 && !parse_mem_width(tok[4], &width))) {
                put_err("pdb: usage: set mem <addr> <value> [size]\n");
                put_err("pdb:        size: b|h|w (or 1|2|4)\n");
                return 1;
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
                    return 1;
                }
                if (width == 2u && (addr & 1u)) {
                    put_err("pdb: set mem halfword requires even address\n");
                    return 1;
                }
                if ((width == 1u && value > 0xffu) ||
                    (width == 2u && value > 0xffffu)) {
                    put_err("pdb: set mem value out of range for size\n");
                    return 1;
                }

                rc = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)base, &old_word);
                if (rc < 0) {
                    put_err("pdb: PEEKDATA failed rc=");
                    put_i32((int32_t)rc);
                    put_chr('\n');
                    return 1;
                }
                value &= mask;
                new_word = (old_word & ~(mask << shift)) | (value << shift);
                rc = ptrace(PTRACE_POKEDATA, pid, (void *)(uintptr_t)base, &new_word);
            }
            if (rc < 0) {
                put_err("pdb: POKEDATA failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                return 1;
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
            return 1;
        }

        put_err("pdb: usage: set reg <name|index> <value>\n");
        put_err("pdb:    or: set mem <addr> <value> [size]\n");
        return 1;
    }

    if (streq(tok[0], "restore")) {
        uint32_t addr = 0;
        uint32_t restored = 0;
        uint8_t bytes[8];
        if (!child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        if (ntok < 4 || !streq(tok[1], "mem") || !parse_u32(tok[2], &addr)) {
            put_err("pdb: usage: restore mem <addr> <byte...>\n");
            return 1;
        }
        for (int i = 3; i < ntok; i++) {
            uint32_t v = 0;
            if (!parse_u32(tok[i], &v) || v > 0xffu) {
                put_err("pdb: usage: restore mem <addr> <byte...>\n");
                restored = 0;
                break;
            }
            bytes[restored] = (uint8_t)v;
            restored++;
        }
        if (restored == 0)
            return 1;
        for (uint32_t i = 0; i < restored; i++) {
            int rc = poke_u8(pid, addr + i, bytes[i]);
            if (rc < 0) {
                put_err("pdb: POKEDATA failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                restored = 0;
                break;
            }
        }
        if (restored == 0)
            return 1;
        put_str("mem ");
        put_hex32(addr);
        put_str(" restored ");
        put_u32(restored);
        put_str(" bytes\n");
        return 1;
    }

    return 0;
}

static int handle_breakpoint_commands(pid_t pid, int child_stopped,
                                      char **tok, int ntok,
                                      struct ppap_ptrace_caps *caps,
                                      pdb_local_bp_t *local_bp)
{
    if (streq(tok[0], "break") || streq(tok[0], "b")) {
        struct ppap_ptrace_bp bp;
        uint32_t requested_flag = 0;
        long rc;
        if (!child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        if (ntok == 2) {
            if (!parse_u32(tok[1], &bp.addr)) {
                put_err("pdb: usage: break <addr>\n");
                return 1;
            }
        } else if (ntok == 3) {
            if (streq(tok[1], "sw")) {
                requested_flag = PPAP_PTRACE_BP_SW;
            } else if (streq(tok[1], "hw")) {
                requested_flag = PPAP_PTRACE_BP_HW;
            } else {
                put_err("pdb: usage: break <addr>\n");
                return 1;
            }
            if (!parse_u32(tok[2], &bp.addr)) {
                put_err("pdb: usage: break <addr>\n");
                return 1;
            }
        } else {
            put_err("pdb: usage: break <addr>\n");
            return 1;
        }
        bp.flags = requested_flag ? requested_flag : PPAP_PTRACE_BP_SW;
        if (ptrace(PTRACE_GETCAPS, pid, (void *)0, caps) == 0) {
            if (requested_flag == PPAP_PTRACE_BP_SW &&
                (caps->caps & PPAP_PTRACE_CAP_SW_BP) == 0u) {
                put_err("pdb: sw break not supported on this target/mapping\n");
                return 1;
            }
            if (requested_flag == PPAP_PTRACE_BP_HW &&
                (caps->caps & PPAP_PTRACE_CAP_HW_BP) == 0u) {
                put_err("pdb: hw break not supported on this target/mapping\n");
                return 1;
            }
            if (!requested_flag) {
                uint32_t cap_flag = select_bp_flag_from_caps(caps->caps);
                if (cap_flag == 0) {
                    put_err("pdb: break not supported on this target/mapping\n");
                    return 1;
                }
                bp.flags = cap_flag;
            }
        }
        bp.id = -1;
        rc = ptrace(PTRACE_SETBP, pid, (void *)0, &bp);
        if (rc < 0) {
            put_err("pdb: SETBP failed rc=");
            put_i32((int32_t)rc);
            put_chr('\n');
            return 1;
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
            local_bp[bp.id].flags = bp.flags;
        }
        return 1;
    }

    if (streq(tok[0], "disable")) {
        struct ppap_ptrace_bp bp;
        uint32_t id = 0;
        long rc;
        if (!child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        if (ntok != 2 || !parse_u32(tok[1], &id)) {
            put_err("pdb: usage: disable <id>\n");
            return 1;
        }
        if (id >= PDB_LOCAL_BP_MAX || !local_bp[id].used) {
            put_err("pdb: unknown breakpoint id\n");
            return 1;
        }
        if (!local_bp[id].enabled) {
            put_str("bp ");
            put_u32(id);
            put_str(" already disabled\n");
            return 1;
        }
        bp.id = (int32_t)id;
        bp.addr = 0;
        bp.flags = 0;
        rc = ptrace(PTRACE_CLRBP, pid, (void *)0, &bp);
        if (rc < 0) {
            put_err("pdb: CLRBP failed rc=");
            put_i32((int32_t)rc);
            put_chr('\n');
            return 1;
        }
        local_bp[id].enabled = 0;
        put_str("bp ");
        put_u32(id);
        put_str(" disabled\n");
        return 1;
    }

    if (streq(tok[0], "enable")) {
        struct ppap_ptrace_bp bp;
        uint32_t id = 0;
        long rc;
        if (!child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        if (ntok != 2 || !parse_u32(tok[1], &id)) {
            put_err("pdb: usage: enable <id>\n");
            return 1;
        }
        if (id >= PDB_LOCAL_BP_MAX || !local_bp[id].used) {
            put_err("pdb: unknown breakpoint id\n");
            return 1;
        }
        if (local_bp[id].enabled) {
            put_str("bp ");
            put_u32(id);
            put_str(" already enabled\n");
            return 1;
        }

        bp.id = -1;
        bp.addr = local_bp[id].addr;
        bp.flags = local_bp[id].flags;
        if (bp.flags == 0) {
            bp.flags = PPAP_PTRACE_BP_SW;
            if (ptrace(PTRACE_GETCAPS, pid, (void *)0, caps) == 0) {
                uint32_t cap_flag = select_bp_flag_from_caps(caps->caps);
                if (cap_flag == 0) {
                    put_err("pdb: break not supported on this target/mapping\n");
                    return 1;
                }
                bp.flags = cap_flag;
            }
        }
        rc = ptrace(PTRACE_SETBP, pid, (void *)0, &bp);
        if (rc < 0) {
            put_err("pdb: SETBP failed rc=");
            put_i32((int32_t)rc);
            put_chr('\n');
            return 1;
        }
        if (bp.id != (int32_t)id) {
            if (bp.id < 0 || bp.id >= PDB_LOCAL_BP_MAX || local_bp[bp.id].used) {
                struct ppap_ptrace_bp rollback;
                rollback.id = bp.id;
                rollback.addr = 0;
                rollback.flags = 0;
                (void)ptrace(PTRACE_CLRBP, pid, (void *)0, &rollback);
                put_err("pdb: enable remap failed\n");
                return 1;
            }
            local_bp[bp.id].used = local_bp[id].used;
            local_bp[bp.id].enabled = 1;
            local_bp[bp.id].addr = local_bp[id].addr;
            local_bp[bp.id].flags = bp.flags;
            local_bp[id].used = 0;
            local_bp[id].enabled = 0;
            local_bp[id].addr = 0;
            local_bp[id].flags = 0;
            put_str("bp ");
            put_u32(id);
            put_str(" enabled as ");
            put_i32(bp.id);
            put_chr('\n');
            return 1;
        }
        local_bp[id].enabled = 1;
        local_bp[id].flags = bp.flags;
        put_str("bp ");
        put_u32(id);
        put_str(" enabled\n");
        return 1;
    }

    if (streq(tok[0], "delete") || streq(tok[0], "d")) {
        struct ppap_ptrace_bp bp;
        uint32_t id = 0;
        long rc;
        if (!child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        if (ntok != 2 || !parse_u32(tok[1], &id)) {
            put_err("pdb: usage: delete <id>\n");
            return 1;
        }
        if (id >= PDB_LOCAL_BP_MAX || !local_bp[id].used) {
            put_err("pdb: unknown breakpoint id\n");
            return 1;
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
                return 1;
            }
        }
        put_str("bp ");
        put_u32(id);
        put_str(" cleared\n");
        if (id < PDB_LOCAL_BP_MAX) {
            local_bp[id].used = 0;
            local_bp[id].enabled = 0;
            local_bp[id].addr = 0;
            local_bp[id].flags = 0;
        }
        return 1;
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
                put_str(" ");
                put_str(bp_flag_name(local_bp[i].flags));
                put_chr('\n');
            }
            if (!found)
                put_str("no breakpoints\n");
            return 1;
        }
        put_err("pdb: usage: info break\n");
        return 1;
    }

    return 0;
}

static int handle_session_commands(pid_t pid, int *child_stopped,
                                   char **tok, int ntok, int *done)
{
    if (streq(tok[0], "detach")) {
        long rc;
        if (ntok != 1) {
            put_err("pdb: usage: detach\n");
            return 1;
        }
        if (!*child_stopped) {
            put_err("pdb: child is not stopped\n");
            return 1;
        }
        rc = ptrace(PTRACE_DETACH, pid, (void *)0, (void *)0);
        if (rc < 0) {
            put_err("pdb: DETACH failed rc=");
            put_i32((int32_t)rc);
            put_chr('\n');
            return 1;
        }
        *child_stopped = 0;
        put_str("detached\n");
        *done = 1;
        return 1;
    }

    if (streq(tok[0], "quit") || streq(tok[0], "q")) {
        if (ntok != 1) {
            put_err("pdb: usage: quit\n");
            return 1;
        }
        if (*child_stopped) {
            long rc = ptrace(PTRACE_DETACH, pid, (void *)0, (void *)0);
            if (rc < 0) {
                put_err("pdb: DETACH failed rc=");
                put_i32((int32_t)rc);
                put_chr('\n');
                return 1;
            }
            *child_stopped = 0;
            put_str("detached\n");
        }
        *done = 1;
        return 1;
    }

    return 0;
}

/* Returns 1 when caller should return with main_exit_code. */
static int dispatch_command(pid_t pid, int *child_stopped,
                            int *child_exit_code, int batch_mode,
                            int *done, int *main_exit_code, char **tok,
                            int ntok, struct ppap_ptrace_event *last_ev,
                            struct ppap_ptrace_caps *caps,
                            pdb_local_bp_t *local_bp)
{
    int run_cmd;

    if (handle_inspect_commands(pid, *child_stopped, tok, ntok, last_ev, caps))
        return 0;

    run_cmd = handle_run_control_commands(pid, child_stopped, child_exit_code,
                                          tok, ntok, last_ev, caps, local_bp,
                                          batch_mode, main_exit_code);
    if (run_cmd == 2)
        return 1;
    if (run_cmd == 1)
        return 0;

    if (handle_write_commands(pid, *child_stopped, tok, ntok))
        return 0;

    if (handle_breakpoint_commands(pid, *child_stopped, tok, ntok, caps, local_bp))
        return 0;

    if (handle_session_commands(pid, child_stopped, tok, ntok, done))
        return 0;

    put_err("pdb: unknown command\n");
    return 0;
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
    int exit_code = 1;
    char line[PDB_SCRIPT_LINE_MAX];
    char *tok[8];
    struct ppap_ptrace_event last_ev;
    struct ppap_ptrace_caps caps;
    pdb_local_bp_t local_bp[PDB_LOCAL_BP_MAX];

    if (!parse_startup_options(argc, argv, &argi, &show_prompt, &batch_mode,
                               &scripted_mode, &attach_mode, &attach_pid,
                               script_cmds, &script_count, pdb_script_storage,
                               &script_storage_used, &exit_code))
        return exit_code;

    if (!validate_startup_options(argc, argi, attach_mode, scripted_mode, script_count))
        return 1;

    init_local_bp_table(local_bp);

    if (!start_tracee(argv, argi, attach_mode, attach_pid, &pid))
        return 1;

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
        if (!read_next_command_line(line, sizeof(line), script_cmds,
                                    script_count, &script_index, show_prompt))
            break;

        int ntok = split_tokens(line, tok, 8);
        if (ntok <= 0)
            continue;

        if (dispatch_command(pid, &child_stopped, &child_exit_code, batch_mode,
                             &done, &exit_code, tok, ntok, &last_ev, &caps,
                             local_bp)) {
            return exit_code;
        }
    }

    return 0;
}
