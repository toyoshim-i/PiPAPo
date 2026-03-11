#include "syscall.h"
#include "common/syscall_nr.h"

static void put_str(const char *s)
{
    int len = 0;
    while (s[len])
        len++;
    write(1, s, len);
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

static void put_hex32(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    put_str("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        put_chr(hex[(v >> shift) & 0xf]);
}

static void put_hex16(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    put_str("0x");
    for (int shift = 12; shift >= 0; shift -= 4)
        put_chr(hex[(v >> shift) & 0xf]);
}

static void put_nl(void)
{
    put_chr('\n');
}

static int streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static const char *event_name(uint32_t event)
{
    switch (event) {
    case PPAP_TRACE_EVENT_EXEC: return "exec";
    case PPAP_TRACE_EVENT_SYSCALL_ENTER: return "enter";
    case PPAP_TRACE_EVENT_SYSCALL_EXIT: return "exit";
    case PPAP_TRACE_EVENT_SUBSYS_ENTER: return "sub-enter";
    case PPAP_TRACE_EVENT_SUBSYS_EXIT: return "sub-exit";
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

static const char *sys_name(uint32_t nr)
{
    switch (nr) {
    case SYS_EXIT: return "exit";
    case SYS_EXECVE: return "execve";
    case SYS_WAITPID: return "waitpid";
    case SYS_GETPID: return "getpid";
    case SYS_PTRACE: return "ptrace";
    case SYS_READ: return "read";
    case SYS_WRITE: return "write";
    case SYS_OPEN: return "open";
    case SYS_CLOSE: return "close";
    case SYS_DUP: return "dup";
    case SYS_DUP2: return "dup2";
    case SYS_PIPE: return "pipe";
    case SYS_IOCTL: return "ioctl";
    case SYS_READV: return "readv";
    case SYS_WRITEV: return "writev";
    case SYS_LSEEK: return "lseek";
    case SYS_STAT: return "stat";
    case SYS_FSTAT: return "fstat";
    case SYS_ACCESS: return "access";
    case SYS_GETCWD: return "getcwd";
    case SYS_MKDIR: return "mkdir";
    case SYS_RMDIR: return "rmdir";
    case SYS_UNLINK: return "unlink";
    case SYS_CHDIR: return "chdir";
    case SYS_GETDENTS: return "getdents";
    case SYS_STAT64: return "stat64";
    case SYS_FSTAT64: return "fstat64";
    case SYS_BRK: return "brk";
    case SYS_MMAP2: return "mmap2";
    case SYS_MUNMAP: return "munmap";
    case SYS_NANOSLEEP: return "nanosleep";
    case SYS_KILL: return "kill";
    case SYS_SIGACTION: return "sigaction";
    case SYS_POLL: return "poll";
    case SYS_PPOLL: return "ppoll";
    default: return (const char *)0;
    }
}

static void print_event(const struct ppap_ptrace_event *ev)
{
    put_str(abi_name(ev->abi));
    put_chr(' ');
    put_str(event_name(ev->event));
    put_chr(' ');

    if (ev->abi == PPAP_TRACE_ABI_PPAP) {
        const char *name = sys_name(ev->nr);
        if (name)
            put_str(name);
        else
            put_hex32(ev->nr);
    } else {
        put_hex32(ev->nr);
    }

    if (ev->event == PPAP_TRACE_EVENT_SYSCALL_ENTER ||
        ev->event == PPAP_TRACE_EVENT_SUBSYS_ENTER) {
        put_str(" a0=");
        put_hex32(ev->args[0]);
        put_str(" a1=");
        put_hex32(ev->args[1]);
        put_str(" a2=");
        put_hex32(ev->args[2]);
    } else if (ev->event == PPAP_TRACE_EVENT_SYSCALL_EXIT ||
               ev->event == PPAP_TRACE_EVENT_SUBSYS_EXIT) {
        put_str(" = ");
        put_hex32((uint32_t)ev->ret);
    }
    put_nl();
}

static void print_regs(const struct ppap_ptrace_regs *regs)
{
    put_str("  ");
    switch (regs->regset) {
    case PPAP_TRACE_REGSET_ARM:
        put_str("arm pc=");
        put_hex32(regs->regs[15]);
        put_str(" sp=");
        put_hex32(regs->regs[13]);
        put_str(" r0=");
        put_hex32(regs->regs[0]);
        put_str(" r1=");
        put_hex32(regs->regs[1]);
        put_str(" r2=");
        put_hex32(regs->regs[2]);
        put_str(" r3=");
        put_hex32(regs->regs[3]);
        break;
    case PPAP_TRACE_REGSET_M68K:
        put_str("m68k pc=");
        put_hex32(regs->regs[16]);
        put_str(" sr=");
        put_hex16(regs->regs[17]);
        put_str(" d0=");
        put_hex32(regs->regs[0]);
        put_str(" d1=");
        put_hex32(regs->regs[1]);
        put_str(" a0=");
        put_hex32(regs->regs[8]);
        put_str(" a7=");
        put_hex32(regs->regs[15]);
        break;
    case PPAP_TRACE_REGSET_Z80:
        put_str("z80 pc=");
        put_hex16(regs->regs[7]);
        put_str(" af=");
        put_hex16(regs->regs[0]);
        put_str(" bc=");
        put_hex16(regs->regs[1]);
        put_str(" de=");
        put_hex16(regs->regs[2]);
        put_str(" hl=");
        put_hex16(regs->regs[3]);
        put_str(" sp=");
        put_hex16(regs->regs[6]);
        break;
    default:
        put_str("regset=");
        put_u32(regs->regset);
        break;
    }
    put_nl();
}

static void usage(void)
{
    put_str("Usage: trace [--ppap] [--subsys] [--both] [--regs] <path> [args...]\n");
}

int main(int argc, char *argv[])
{
    int dump_regs = 0;
    int mode_seen = 0;
    uint32_t mode = 0;
    int cmd = 1;

    while (cmd < argc && argv[cmd][0] == '-') {
        if (streq(argv[cmd], "--ppap")) {
            if (!mode_seen)
                mode = 0;
            mode_seen = 1;
            mode |= PPAP_TRACE_MODE_PPAP_SYSCALL;
        } else if (streq(argv[cmd], "--subsys")) {
            if (!mode_seen)
                mode = 0;
            mode_seen = 1;
            mode |= PPAP_TRACE_MODE_SUBSYS_CALL;
        } else if (streq(argv[cmd], "--both")) {
            mode_seen = 1;
            mode = PPAP_TRACE_MODE_PPAP_SYSCALL | PPAP_TRACE_MODE_SUBSYS_CALL;
        } else if (streq(argv[cmd], "--regs")) {
            dump_regs = 1;
        } else {
            usage();
            return 1;
        }
        cmd++;
    }

    if (cmd >= argc) {
        usage();
        return 1;
    }
    if (!mode_seen)
        mode = PPAP_TRACE_MODE_PPAP_SYSCALL | PPAP_TRACE_MODE_SUBSYS_CALL;

    pid_t pid = vfork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, (void *)0, (void *)0);
        execve(argv[cmd], &argv[cmd], (void *)0);
        _exit(127);
    }

    for (;;) {
        int status = 0;
        struct ppap_ptrace_event ev;
        struct ppap_ptrace_regs regs;

        if (waitpid(pid, &status, WSTOPPED) != pid) {
            put_str("trace: waitpid failed\n");
            return 1;
        }
        if (WIFEXITED(status)) {
            put_str("exit ");
            put_u32((uint32_t)WEXITSTATUS(status));
            put_nl();
            return WEXITSTATUS(status);
        }
        if (!WIFSTOPPED(status)) {
            put_str("trace: unexpected child state\n");
            return 1;
        }

        if (ptrace(PTRACE_GETEVENT, pid, (void *)0, &ev) < 0) {
            put_str("trace: GETEVENT failed\n");
            return 1;
        }
        print_event(&ev);

        if (dump_regs) {
            if (ptrace(PTRACE_GETREGS, pid, (void *)0, &regs) < 0) {
                put_str("trace: GETREGS failed\n");
                return 1;
            }
            print_regs(&regs);
        }

        if (ptrace(PTRACE_SETMODE, pid, (void *)(uintptr_t)mode, (void *)0) < 0) {
            put_str("trace: SETMODE failed\n");
            return 1;
        }
        if (ptrace(PTRACE_CONT, pid, (void *)0, (void *)0) < 0) {
            put_str("trace: CONT failed\n");
            return 1;
        }
    }
}
