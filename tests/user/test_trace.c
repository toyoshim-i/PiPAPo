/*
 * test_trace.c — Minimal ptrace + waitpid(WSTOPPED) integration test
 *
 * Verifies:
 *   1. PTRACE_TRACEME stops the child after execve()
 *   2. PTRACE_SETMODE enables PPAP syscall tracing
 *   3. PTRACE_CONT preserves the active trace mode
 *   4. PTRACE_PEEKDATA / PTRACE_POKEDATA inspect and modify tracee memory
 *   5. PTRACE_SETMODE(0) disables tracing before final continue
 */

#include "utest.h"
#include "common/syscall_nr.h"

#define SIGTRAP  5

#if defined(__m68k__)
#define EXPECT_REGSET  PPAP_TRACE_REGSET_M68K
#else
#define EXPECT_REGSET  PPAP_TRACE_REGSET_ARM
#endif

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
    uint32_t word = 0;
    uint32_t patched = 0x55667788u;

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

    UT_SUMMARY("test_trace");
}
