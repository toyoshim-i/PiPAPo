/*
 * test_trace.c — Minimal ptrace + waitpid(WSTOPPED) integration test
 *
 * Verifies:
 *   1. PTRACE_TRACEME stops the child after execve()
 *   2. PTRACE_SETMODE enables PPAP syscall tracing
 *   3. PTRACE_CONT preserves the active trace mode
 *   4. PTRACE_SETMODE(0) disables tracing before final continue
 */

#include "utest.h"
#include "common/syscall_nr.h"

#define SIGTRAP  5

int main(void)
{
    pid_t pid = vfork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, (void *)0, (void *)0);
        execve("/bin/hello", (void *)0, (void *)0);
        _exit(127);
    }

    int status = 0;
    struct ppap_ptrace_event ev;

    UT_ASSERT_EQ(waitpid(pid, &status, WSTOPPED), pid);
    UT_ASSERT(WIFSTOPPED(status), "child should stop after exec");
    UT_ASSERT_EQ(WSTOPSIG(status), SIGTRAP);
    UT_ASSERT_EQ(ptrace(PTRACE_GETEVENT, pid, (void *)0, &ev), 0);
    UT_ASSERT_EQ((int)ev.event, PPAP_TRACE_EVENT_EXEC);
    UT_ASSERT_EQ((int)ev.abi, PPAP_TRACE_ABI_PPAP);

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

    UT_ASSERT_EQ(ptrace(PTRACE_CONT, pid, (void *)0, (void *)0), 0);
    UT_ASSERT_EQ(waitpid(pid, &status, WSTOPPED), pid);
    UT_ASSERT(WIFSTOPPED(status), "child should stop at syscall exit");
    UT_ASSERT_EQ(ptrace(PTRACE_GETEVENT, pid, (void *)0, &ev), 0);
    UT_ASSERT_EQ((int)ev.event, PPAP_TRACE_EVENT_SYSCALL_EXIT);
    UT_ASSERT_EQ((int)ev.abi, PPAP_TRACE_ABI_PPAP);
    UT_ASSERT_EQ((int)ev.nr, SYS_WRITE);
    UT_ASSERT(ev.ret > 0, "write should return positive length");

    UT_ASSERT_EQ(ptrace(PTRACE_SETMODE, pid, (void *)0, (void *)0), 0);
    UT_ASSERT_EQ(ptrace(PTRACE_CONT, pid, (void *)0, (void *)0), 0);
    UT_ASSERT_EQ(waitpid(pid, &status, 0), pid);
    UT_ASSERT(WIFEXITED(status), "child should exit after continue");
    UT_ASSERT_EQ(WEXITSTATUS(status), 0);

    UT_SUMMARY("test_trace");
}
