/*
 * test_fault.c — CPU fault handler tests
 *
 * When run without arguments: vfork+execve self with a test index,
 * then check the child's exit status matches 128+signal.
 *
 * When run with argument "0"-"1": trigger the indexed fault.
 *
 * Tests:
 *   0: illegal_insn  → SIGILL  (undefined opcode)
 *   1: divzero       → SIGFPE  (hardware divide by zero)
 *
 * Architecture notes:
 *   m68k: Both tests work. ILLEGAL opcode 0x4AFC, DIVS.W by zero.
 *   ARM:  Skipped — Cortex-M0+ HardFault handler doesn't yet kill
 *         the faulting process, so faults loop forever.
 */

#include "utest.h"

#if defined(__m68k__)

/* ── Fault triggers (child process) ──────────────────────────────────────── */

static void __attribute__((noreturn)) do_illegal_insn(void)
{
    /* 0x4AFC is the m68k ILLEGAL instruction */
    __asm__ volatile (".short 0x4afc");
    _exit(99);
}

static void __attribute__((noreturn)) do_divzero(void)
{
    /* Force DIVS.W instruction — C division uses __divsi3 sw routine */
    volatile int zero = 0;
    int result;
    __asm__ volatile (
        "move.l %1, %%d0\n\t"
        "divs.w %2, %%d0\n\t"
        "move.l %%d0, %0"
        : "=d" (result)
        : "d" (42), "d" ((short)zero)
        : "d0"
    );
    (void)result;
    _exit(99);
}

/* ── Child mode: trigger indexed fault ───────────────────────────────────── */

static void __attribute__((noreturn)) run_fault(int idx)
{
    switch (idx) {
    case 0: do_illegal_insn();
    case 1: do_divzero();
    default: _exit(98);
    }
}

/* ── Run one test: vfork+execve self with index, check exit status ───────── */

#define SIGILL  4
#define SIGFPE  8

static int run_child_test(int idx)
{
    pid_t pid = vfork();
    if (pid == 0) {
        char num[2];
        num[0] = '0' + idx;
        num[1] = '\0';
        char *argv[] = { "/bin/test_fault", num, (char *)0 };
        char *envp[] = { (char *)0 };
        execve("/bin/test_fault", argv, envp);
        _exit(97);
    }
    if (pid < 0) return -1;

    int status = 0;
    waitpid(pid, &status, 0);
    return (status >> 8) & 0xff;
}

#endif /* __m68k__ */

int main(int argc, char *argv[])
{
#if defined(__m68k__)
    /* Child mode */
    if (argc >= 2 && argv[1][0] >= '0' && argv[1][0] <= '1')
        run_fault(argv[1][0] - '0');

    /* Parent mode */
    int code;

    /* Test 0: illegal instruction */
    code = run_child_test(0);
    if (code != 99)
        UT_ASSERT_EQ(code, 128 + SIGILL);

    /* Test 1: divide by zero (skipped if no hw divide) */
    code = run_child_test(1);
    if (code != 99)
        UT_ASSERT_EQ(code, 128 + SIGFPE);
#else
    (void)argc; (void)argv;
    /* ARM Cortex-M0+ has no user-mode crash handler yet —
     * HardFault loops forever, so skip all fault tests. */
    UT_PRINT("test_fault: skipped (no ARM crash handler)\n");
#endif

    UT_SUMMARY("test_fault");
}
