/*
 * test_trace.c — Minimal ptrace + waitpid(WSTOPPED) integration test
 *
 * Verifies:
 *   1. PTRACE_TRACEME stops the child after execve()
 *   2. PTRACE_SETMODE enables PPAP syscall tracing
 *   3. PTRACE_CONT preserves the active trace mode
 *   4. PTRACE_GETCAPS reports debugger capabilities
 *   5. PTRACE_SETREGS updates a stopped tracee register
 *   6. PTRACE_PEEKDATA / PTRACE_POKEDATA inspect and modify tracee memory
 *   7. PTRACE_SINGLESTEP stops an eCPU tracee after one guest instruction
 *   8. PTRACE_SETBP / PTRACE_CLRBP manage software breakpoints on eCPU
 *   9. PTRACE_SETSURFACE switches ptrace view between real/eCPU surfaces
 *  10. PTRACE_SETMODE(0) disables tracing before final continue
 *  11. PTRACE_SINGLESTEP works for m68k eCPU tracees on ARM targets
 */

#include "utest.h"
#include "common/syscall_nr.h"

#define SIGTRAP  5

#if defined(__m68k__)
#define EXPECT_REGSET  PPAP_TRACE_REGSET_M68K
#define SCRATCH_REG_IDX  7   /* d7 */
#else
#define EXPECT_REGSET  PPAP_TRACE_REGSET_ARM
#define SCRATCH_REG_IDX  12  /* r12 */
#endif

static int write_blob(const char *path, const uint8_t *data, int size)
{
    int fd;
    int n;

    unlink(path);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0)
        return -1;
    n = write(fd, data, (size_t)size);
    close(fd);
    if (n != size) {
        unlink(path);
        return -1;
    }
    return 0;
}

#if !defined(__m68k__)
static int run_exit_code(const char *path)
{
    pid_t pid = vfork();
    if (pid == 0) {
        execve(path, (void *)0, (void *)0);
        _exit(127);
    }
    if (pid < 0)
        return 127;

    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return 127;
    return (status >> 8) & 0xff;
}
#endif

static const uint8_t trace_step_com[] = {
    0x00,                   /* NOP */
    0x00,                   /* NOP */
    0x0E, 0x00,             /* LD C,0 (BDOS exit) */
    0xCD, 0x05, 0x00,       /* CALL 0005h */
};

static const uint8_t trace_swbp_com[] = {
    0x00,                   /* NOP @ 0100 */
    0x00,                   /* NOP @ 0101 */
    0x00,                   /* NOP @ 0102 */
    0x0E, 0x00,             /* LD C,0 (BDOS exit) */
    0xCD, 0x05, 0x00,       /* CALL 0005h */
};

int main(void)
{
    pid_t pid = vfork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, (void *)0, (void *)0);
        execve("/bin/trace_peek_target", (void *)0, (void *)0);
        _exit(127);
    }

    int status = 0;
    struct ppap_ptrace_event ev;
    struct ppap_ptrace_regs regs;
    struct ppap_ptrace_regs set_regs;
    struct ppap_ptrace_caps caps;
    struct ppap_ptrace_bp bp;
    uint32_t surface = 0;
    uint32_t word = 0;
    uint32_t patched = 0x55667788u;
    uint32_t scratch_before = 0;
    uint32_t z80_pc_before = 0;
#if !defined(__m68k__)
    uint32_t m68k_pc_before = 0;
#endif

    UT_ASSERT_EQ(waitpid(pid, &status, WSTOPPED), pid);
    UT_ASSERT(WIFSTOPPED(status), "child should stop after exec");
    UT_ASSERT_EQ(WSTOPSIG(status), SIGTRAP);
    UT_ASSERT_EQ(ptrace(PTRACE_GETEVENT, pid, (void *)0, &ev), 0);
    UT_ASSERT_EQ((int)ev.event, PPAP_TRACE_EVENT_EXEC);
    UT_ASSERT_EQ((int)ev.abi, PPAP_TRACE_ABI_PPAP);
    UT_ASSERT_EQ(ptrace(PTRACE_GETREGS, pid, (void *)0, &regs), 0);
    UT_ASSERT_EQ((int)regs.regset, EXPECT_REGSET);
    UT_ASSERT_EQ((int)regs.abi, PPAP_TRACE_ABI_PPAP);
    UT_ASSERT(regs.words > 0, "GETREGS should return a non-empty register set");

    UT_ASSERT_EQ(ptrace(PTRACE_GETCAPS, pid, (void *)0, &caps), 0);
    UT_ASSERT_EQ((int)caps.regset, EXPECT_REGSET);
    UT_ASSERT_EQ((int)caps.abi, PPAP_TRACE_ABI_PPAP);
    UT_ASSERT_EQ((int)caps.surface, PPAP_TRACE_SURFACE_REAL);
    UT_ASSERT((caps.surfaces & PPAP_PTRACE_SURFACE_MASK_REAL) != 0,
              "native tracee should expose real surface");
    UT_ASSERT((caps.surfaces & PPAP_PTRACE_SURFACE_MASK_ECPU) == 0,
              "native tracee should not expose ecpu surface");
    UT_ASSERT((caps.caps & PPAP_PTRACE_CAP_GETREGS) != 0,
              "GETCAPS should include GETREGS capability");
    UT_ASSERT((caps.caps & PPAP_PTRACE_CAP_SETREGS) != 0,
              "GETCAPS should include SETREGS capability");
    UT_ASSERT((caps.caps & PPAP_PTRACE_CAP_PEEKPOKE) != 0,
              "GETCAPS should include PEEK/POKE capability");
    UT_ASSERT_EQ(ptrace(PTRACE_GETSURFACE, pid, (void *)0, &surface), 0);
    UT_ASSERT_EQ((int)surface, PPAP_TRACE_SURFACE_REAL);
    UT_ASSERT_EQ(ptrace(PTRACE_SETSURFACE, pid,
                        (void *)(uintptr_t)PPAP_TRACE_SURFACE_REAL,
                        (void *)0), 0);
    UT_ASSERT_EQ(ptrace(PTRACE_GETSURFACE, pid, (void *)0, &surface), 0);
    UT_ASSERT_EQ((int)surface, PPAP_TRACE_SURFACE_REAL);
    UT_ASSERT(ptrace(PTRACE_SETSURFACE, pid,
                     (void *)(uintptr_t)PPAP_TRACE_SURFACE_ECPU,
                     (void *)0) < 0,
              "native tracee should reject eCPU surface");

    set_regs.regset = regs.regset;
    set_regs.abi = regs.abi;
    set_regs.words = regs.words;
    for (uint32_t i = 0; i < PPAP_PTRACE_REGS_MAX; i++)
        set_regs.regs[i] = regs.regs[i];
    scratch_before = set_regs.regs[SCRATCH_REG_IDX];
    set_regs.regs[SCRATCH_REG_IDX] ^= 0x00A55A5Au;
    if (set_regs.regs[SCRATCH_REG_IDX] == scratch_before)
        set_regs.regs[SCRATCH_REG_IDX]++;
    UT_ASSERT_EQ(ptrace(PTRACE_SETREGS, pid, (void *)0, &set_regs), 0);
    UT_ASSERT_EQ(ptrace(PTRACE_GETREGS, pid, (void *)0, &regs), 0);
    UT_ASSERT(regs.regs[SCRATCH_REG_IDX] == set_regs.regs[SCRATCH_REG_IDX],
              "SETREGS should update the selected register");

    set_regs.regset = regs.regset;
    set_regs.abi = regs.abi;
    set_regs.words = regs.words;
    for (uint32_t i = 0; i < PPAP_PTRACE_REGS_MAX; i++)
        set_regs.regs[i] = regs.regs[i];
    set_regs.regs[SCRATCH_REG_IDX] = scratch_before;
    UT_ASSERT_EQ(ptrace(PTRACE_SETREGS, pid, (void *)0, &set_regs), 0);

    UT_ASSERT_EQ(ptrace(PTRACE_SETMODE, pid,
                        (void *)(uintptr_t)PPAP_TRACE_MODE_PPAP_SYSCALL,
                        (void *)0), 0);
    UT_ASSERT_EQ(ptrace(PTRACE_CONT, pid, (void *)0, (void *)0), 0);
    UT_ASSERT_EQ(waitpid(pid, &status, WSTOPPED), pid);
    UT_ASSERT(WIFSTOPPED(status), "child should stop at syscall entry");
    UT_ASSERT_EQ(ptrace(PTRACE_GETEVENT, pid, (void *)0, &ev), 0);
    UT_ASSERT_EQ((int)ev.event, PPAP_TRACE_EVENT_SYSCALL_ENTER);
    UT_ASSERT_EQ((int)ev.abi, PPAP_TRACE_ABI_PPAP);
    UT_ASSERT_EQ((int)ev.nr, SYS_WRITE);
    UT_ASSERT_EQ((int)ev.args[2], 4);
    UT_ASSERT_EQ(ptrace(PTRACE_GETREGS, pid, (void *)0, &regs), 0);
    UT_ASSERT_EQ((int)regs.regset, EXPECT_REGSET);
    UT_ASSERT_EQ((int)regs.abi, PPAP_TRACE_ABI_PPAP);
    UT_ASSERT_EQ(ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)ev.args[1],
                        &word), 0);
    UT_ASSERT_EQ((int)word, 0x11223344);
    UT_ASSERT_EQ(ptrace(PTRACE_POKEDATA, pid, (void *)(uintptr_t)ev.args[1],
                        &patched), 0);
    word = 0;
    UT_ASSERT_EQ(ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)ev.args[1],
                        &word), 0);
    UT_ASSERT_EQ((int)word, (int)patched);

    UT_ASSERT_EQ(ptrace(PTRACE_CONT, pid, (void *)0, (void *)0), 0);
    UT_ASSERT_EQ(waitpid(pid, &status, WSTOPPED), pid);
    UT_ASSERT(WIFSTOPPED(status), "child should stop at syscall exit");
    UT_ASSERT_EQ(ptrace(PTRACE_GETEVENT, pid, (void *)0, &ev), 0);
    UT_ASSERT_EQ((int)ev.event, PPAP_TRACE_EVENT_SYSCALL_EXIT);
    UT_ASSERT_EQ((int)ev.abi, PPAP_TRACE_ABI_PPAP);
    UT_ASSERT_EQ((int)ev.nr, SYS_WRITE);
    UT_ASSERT(ev.ret > 0, "write should return positive length");
    UT_ASSERT_EQ(ptrace(PTRACE_GETREGS, pid, (void *)0, &regs), 0);
    UT_ASSERT_EQ((int)regs.regset, EXPECT_REGSET);
    UT_ASSERT_EQ((int)regs.abi, PPAP_TRACE_ABI_PPAP);

    UT_ASSERT_EQ(ptrace(PTRACE_SETMODE, pid, (void *)0, (void *)0), 0);
    UT_ASSERT_EQ(ptrace(PTRACE_CONT, pid, (void *)0, (void *)0), 0);
    UT_ASSERT_EQ(waitpid(pid, &status, 0), pid);
    UT_ASSERT(WIFEXITED(status), "child should exit after continue");
    UT_ASSERT_EQ(WEXITSTATUS(status), 0);

    UT_ASSERT(ptrace(PTRACE_ATTACH, 0, (void *)0, (void *)0) < 0,
              "PTRACE_ATTACH should reject pid 0");
    UT_ASSERT(ptrace(PTRACE_ATTACH, getpid(), (void *)0, (void *)0) < 0,
              "PTRACE_ATTACH should reject self-attach");

    {
        char *sleep_argv[3];

        sleep_argv[0] = (char *)"/bin/sleep";
        sleep_argv[1] = (char *)"1";
        sleep_argv[2] = (char *)0;

        pid = vfork();
        if (pid == 0) {
            execve("/bin/sleep", sleep_argv, (void *)0);
            _exit(127);
        }

        UT_ASSERT(pid > 0, "attach target should launch");
        UT_ASSERT_EQ(ptrace(PTRACE_ATTACH, pid, (void *)0, (void *)0), 0);
        UT_ASSERT(ptrace(PTRACE_ATTACH, pid, (void *)0, (void *)0) < 0,
                  "PTRACE_ATTACH should reject already-traced target");
        UT_ASSERT_EQ(waitpid(pid, &status, WSTOPPED), pid);
        UT_ASSERT(WIFSTOPPED(status), "attached target should stop");
        UT_ASSERT_EQ(WSTOPSIG(status), SIGTRAP);
        UT_ASSERT_EQ(ptrace(PTRACE_GETEVENT, pid, (void *)0, &ev), 0);
        UT_ASSERT_EQ((int)ev.event, PPAP_TRACE_EVENT_DEBUG_STOP);

        UT_ASSERT_EQ(ptrace(PTRACE_DETACH, pid, (void *)0, (void *)0), 0);
        UT_ASSERT_EQ(waitpid(pid, &status, 0), pid);
        UT_ASSERT(WIFEXITED(status), "detached target should exit");
        UT_ASSERT_EQ(WEXITSTATUS(status), 0);
    }

    UT_ASSERT_EQ(write_blob("/tmp/trace_step.com", trace_step_com,
                            (int)sizeof(trace_step_com)), 0);

    pid = vfork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, (void *)0, (void *)0);
        execve("/tmp/trace_step.com", (void *)0, (void *)0);
        _exit(127);
    }

    UT_ASSERT_EQ(waitpid(pid, &status, WSTOPPED), pid);
    UT_ASSERT(WIFSTOPPED(status), "CP/M child should stop after exec");
    UT_ASSERT_EQ(WSTOPSIG(status), SIGTRAP);
    UT_ASSERT_EQ(ptrace(PTRACE_GETEVENT, pid, (void *)0, &ev), 0);
    UT_ASSERT_EQ((int)ev.event, PPAP_TRACE_EVENT_EXEC);

    UT_ASSERT_EQ(ptrace(PTRACE_GETCAPS, pid, (void *)0, &caps), 0);
    UT_ASSERT_EQ((int)caps.regset, PPAP_TRACE_REGSET_Z80);
    UT_ASSERT_EQ((int)caps.surface, PPAP_TRACE_SURFACE_ECPU);
    UT_ASSERT((caps.surfaces & PPAP_PTRACE_SURFACE_MASK_REAL) != 0,
              "CP/M tracee should expose real surface");
    UT_ASSERT((caps.surfaces & PPAP_PTRACE_SURFACE_MASK_ECPU) != 0,
              "CP/M tracee should expose ecpu surface");
    UT_ASSERT((caps.caps & PPAP_PTRACE_CAP_SINGLESTEP) != 0,
              "CP/M tracee should report single-step capability");
    UT_ASSERT_EQ(ptrace(PTRACE_GETSURFACE, pid, (void *)0, &surface), 0);
    UT_ASSERT_EQ((int)surface, PPAP_TRACE_SURFACE_ECPU);
    UT_ASSERT_EQ(ptrace(PTRACE_SETSURFACE, pid,
                        (void *)(uintptr_t)PPAP_TRACE_SURFACE_REAL,
                        (void *)0), 0);
    UT_ASSERT_EQ(ptrace(PTRACE_GETSURFACE, pid, (void *)0, &surface), 0);
    UT_ASSERT_EQ((int)surface, PPAP_TRACE_SURFACE_REAL);
    UT_ASSERT_EQ(ptrace(PTRACE_GETCAPS, pid, (void *)0, &caps), 0);
    UT_ASSERT_EQ((int)caps.regset, EXPECT_REGSET);
    UT_ASSERT_EQ((int)caps.surface, PPAP_TRACE_SURFACE_REAL);
    UT_ASSERT((caps.caps & PPAP_PTRACE_CAP_SINGLESTEP) == 0,
              "real surface should hide eCPU single-step capability");
    UT_ASSERT((caps.caps & PPAP_PTRACE_CAP_SW_BP) == 0,
              "real surface should hide eCPU software breakpoint capability");
    UT_ASSERT_EQ(ptrace(PTRACE_SETSURFACE, pid,
                        (void *)(uintptr_t)PPAP_TRACE_SURFACE_ECPU,
                        (void *)0), 0);
    UT_ASSERT_EQ(ptrace(PTRACE_GETSURFACE, pid, (void *)0, &surface), 0);
    UT_ASSERT_EQ((int)surface, PPAP_TRACE_SURFACE_ECPU);
    UT_ASSERT_EQ(ptrace(PTRACE_GETCAPS, pid, (void *)0, &caps), 0);
    UT_ASSERT_EQ((int)caps.regset, PPAP_TRACE_REGSET_Z80);
    UT_ASSERT_EQ((int)caps.surface, PPAP_TRACE_SURFACE_ECPU);

    UT_ASSERT_EQ(ptrace(PTRACE_GETREGS, pid, (void *)0, &regs), 0);
    UT_ASSERT_EQ((int)regs.regset, PPAP_TRACE_REGSET_Z80);
    z80_pc_before = regs.regs[7];

    UT_ASSERT_EQ(ptrace(PTRACE_SINGLESTEP, pid, (void *)0, (void *)0), 0);
    UT_ASSERT_EQ(waitpid(pid, &status, WSTOPPED), pid);
    UT_ASSERT(WIFSTOPPED(status), "CP/M child should stop after single-step");
    UT_ASSERT_EQ(WSTOPSIG(status), SIGTRAP);
    UT_ASSERT_EQ(ptrace(PTRACE_GETEVENT, pid, (void *)0, &ev), 0);
    UT_ASSERT_EQ((int)ev.event, PPAP_TRACE_EVENT_DEBUG_STOP);
    UT_ASSERT((ev.flags & PPAP_DEBUG_STOP_STEP) != 0,
              "debug stop should include STEP reason");
    UT_ASSERT_EQ(ptrace(PTRACE_GETREGS, pid, (void *)0, &regs), 0);
    UT_ASSERT_EQ((int)regs.regset, PPAP_TRACE_REGSET_Z80);
    UT_ASSERT(regs.regs[7] != z80_pc_before, "single-step should advance PC");
    UT_ASSERT_EQ((int)ev.args[0], (int)regs.regs[7]);

    UT_ASSERT_EQ(ptrace(PTRACE_CONT, pid, (void *)0, (void *)0), 0);
    UT_ASSERT_EQ(waitpid(pid, &status, 0), pid);
    UT_ASSERT(WIFEXITED(status), "CP/M child should exit after continue");
    UT_ASSERT_EQ(WEXITSTATUS(status), 0);
    unlink("/tmp/trace_step.com");

#if !defined(__m68k__)
    if (run_exit_code("/bin/hello_m68k") == 0) {
        pid = vfork();
        if (pid == 0) {
            ptrace(PTRACE_TRACEME, 0, (void *)0, (void *)0);
            execve("/bin/hello_m68k", (void *)0, (void *)0);
            _exit(127);
        }

        UT_ASSERT_EQ(waitpid(pid, &status, WSTOPPED), pid);
        UT_ASSERT(WIFSTOPPED(status), "m68k eCPU child should stop after exec");
        UT_ASSERT_EQ(WSTOPSIG(status), SIGTRAP);
        UT_ASSERT_EQ(ptrace(PTRACE_GETEVENT, pid, (void *)0, &ev), 0);
        UT_ASSERT_EQ((int)ev.event, PPAP_TRACE_EVENT_EXEC);
        UT_ASSERT_EQ((int)ev.abi, PPAP_TRACE_ABI_PPAP);

        UT_ASSERT_EQ(ptrace(PTRACE_GETCAPS, pid, (void *)0, &caps), 0);
        UT_ASSERT_EQ((int)caps.regset, PPAP_TRACE_REGSET_M68K);
        UT_ASSERT_EQ((int)caps.surface, PPAP_TRACE_SURFACE_ECPU);
        UT_ASSERT((caps.surfaces & PPAP_PTRACE_SURFACE_MASK_REAL) != 0,
                  "m68k eCPU tracee should expose real surface");
        UT_ASSERT((caps.surfaces & PPAP_PTRACE_SURFACE_MASK_ECPU) != 0,
                  "m68k eCPU tracee should expose ecpu surface");
        UT_ASSERT((caps.caps & PPAP_PTRACE_CAP_SINGLESTEP) != 0,
                  "m68k eCPU tracee should report single-step capability");

        UT_ASSERT_EQ(ptrace(PTRACE_GETREGS, pid, (void *)0, &regs), 0);
        UT_ASSERT_EQ((int)regs.regset, PPAP_TRACE_REGSET_M68K);
        m68k_pc_before = regs.regs[16]; /* PC in m68k regset */

        UT_ASSERT_EQ(ptrace(PTRACE_SINGLESTEP, pid, (void *)0, (void *)0), 0);
        UT_ASSERT_EQ(waitpid(pid, &status, WSTOPPED), pid);
        UT_ASSERT(WIFSTOPPED(status),
                  "m68k eCPU child should stop after single-step");
        UT_ASSERT_EQ(WSTOPSIG(status), SIGTRAP);
        UT_ASSERT_EQ(ptrace(PTRACE_GETEVENT, pid, (void *)0, &ev), 0);
        UT_ASSERT_EQ((int)ev.event, PPAP_TRACE_EVENT_DEBUG_STOP);
        UT_ASSERT((ev.flags & PPAP_DEBUG_STOP_STEP) != 0,
                  "m68k eCPU debug stop should include STEP reason");
        UT_ASSERT_EQ(ptrace(PTRACE_GETREGS, pid, (void *)0, &regs), 0);
        UT_ASSERT_EQ((int)regs.regset, PPAP_TRACE_REGSET_M68K);
        UT_ASSERT(regs.regs[16] != m68k_pc_before,
                  "m68k eCPU single-step should advance PC");
        UT_ASSERT_EQ((int)ev.args[0], (int)regs.regs[16]);

        UT_ASSERT_EQ(ptrace(PTRACE_CONT, pid, (void *)0, (void *)0), 0);
        UT_ASSERT_EQ(waitpid(pid, &status, 0), pid);
        UT_ASSERT(WIFEXITED(status),
                  "m68k eCPU child should exit after continue");
        UT_ASSERT_EQ(WEXITSTATUS(status), 0);
    } else {
        UT_ASSERT(1, "skip m68k eCPU ptrace step: /bin/hello_m68k unavailable");
    }
#endif

    UT_ASSERT_EQ(write_blob("/tmp/trace_swbp.com", trace_swbp_com,
                            (int)sizeof(trace_swbp_com)), 0);

    pid = vfork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, (void *)0, (void *)0);
        execve("/tmp/trace_swbp.com", (void *)0, (void *)0);
        _exit(127);
    }

    UT_ASSERT_EQ(waitpid(pid, &status, WSTOPPED), pid);
    UT_ASSERT(WIFSTOPPED(status), "CP/M swbp child should stop after exec");
    UT_ASSERT_EQ(ptrace(PTRACE_GETCAPS, pid, (void *)0, &caps), 0);
    UT_ASSERT((caps.caps & PPAP_PTRACE_CAP_SW_BP) != 0,
              "CP/M tracee should report SW breakpoint capability");
    UT_ASSERT(caps.max_bps > 0, "GETCAPS should report non-zero bp budget");

    bp.id = -1;
    bp.addr = 0x0101;
    bp.flags = PPAP_PTRACE_BP_SW;
    UT_ASSERT_EQ(ptrace(PTRACE_SETBP, pid, (void *)0, &bp), 0);
    UT_ASSERT(bp.id >= 0, "SETBP should return a non-negative breakpoint ID");

    UT_ASSERT_EQ(ptrace(PTRACE_CONT, pid, (void *)0, (void *)0), 0);
    UT_ASSERT_EQ(waitpid(pid, &status, WSTOPPED), pid);
    UT_ASSERT(WIFSTOPPED(status), "CP/M swbp child should stop at breakpoint");
    UT_ASSERT_EQ(ptrace(PTRACE_GETEVENT, pid, (void *)0, &ev), 0);
    UT_ASSERT_EQ((int)ev.event, PPAP_TRACE_EVENT_DEBUG_STOP);
    UT_ASSERT((ev.flags & PPAP_DEBUG_STOP_SW_BP) != 0,
              "debug stop should include SW_BP reason");
    UT_ASSERT_EQ((int)ev.args[0], 0x0101);

    UT_ASSERT_EQ(ptrace(PTRACE_GETREGS, pid, (void *)0, &regs), 0);
    UT_ASSERT_EQ((int)regs.regset, PPAP_TRACE_REGSET_Z80);
    UT_ASSERT_EQ((int)regs.regs[7], 0x0101);

    UT_ASSERT_EQ(ptrace(PTRACE_CLRBP, pid, (void *)0, &bp), 0);
    UT_ASSERT_EQ(ptrace(PTRACE_CONT, pid, (void *)0, (void *)0), 0);
    UT_ASSERT_EQ(waitpid(pid, &status, 0), pid);
    UT_ASSERT(WIFEXITED(status), "CP/M swbp child should exit after continue");
    UT_ASSERT_EQ(WEXITSTATUS(status), 0);
    unlink("/tmp/trace_swbp.com");

    UT_SUMMARY("test_trace");
}
