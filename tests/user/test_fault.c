/*
 * test_fault.c — CPU fault handler tests
 *
 * When run without arguments: vfork+execve self with a test index,
 * then check the child's exit status matches 128+signal.
 *
 * When run with argument "0"-"3": trigger the indexed fault.
 *
 * Tests:
 *   0: bus_error      → SIGBUS  (read from unmapped address)
 *   1: illegal_insn   → SIGILL  (m68k ILLEGAL opcode 0x4AFC)
 *   2: divzero        → SIGFPE  (DIVS.W by zero)
 *   3: odd_address    → SIGBUS  (word read from odd address)
 */

#include "utest.h"

/* ── Fault triggers (child process) ──────────────────────────────────────── */

static void __attribute__((noreturn)) do_bus_error(void)
{
    /* Read from an unmapped high address.
     * Note: QEMU virt may not enforce bus errors — test may SKIP. */
    volatile int *p = (volatile int *)0xDEAD0000;
    (void)*p;
    _exit(99);  /* reached = no fault triggered */
}

static void __attribute__((noreturn)) do_illegal_insn(void)
{
    __asm__ volatile (".short 0x4afc");
    _exit(99);
}

static void __attribute__((noreturn)) do_divzero(void)
{
    /* Force DIVS.W instruction (C division uses __divsi3 sw routine) */
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

static void __attribute__((noreturn)) do_odd_address(void)
{
    /* Word read from odd address → address error on 68000.
     * Note: QEMU m68000 doesn't enforce alignment — test may SKIP. */
    volatile short *p = (volatile short *)0x1001;
    (void)*p;
    _exit(99);
}

/* ── Child mode: trigger indexed fault ───────────────────────────────────── */

static void __attribute__((noreturn)) run_fault(int idx)
{
    switch (idx) {
    case 0: do_bus_error();
    case 1: do_illegal_insn();
    case 2: do_divzero();
    case 3: do_odd_address();
    default: _exit(98);
    }
}

/* ── Run one test: vfork+execve self with index, check exit status ───────── */

#define SIGILL  4
#define SIGBUS  7
#define SIGFPE  8

static int run_child_test(int idx, int expected_sig)
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

int main(int argc, char *argv[])
{
    /* Child mode */
    if (argc >= 2 && argv[1][0] >= '0' && argv[1][0] <= '3')
        run_fault(argv[1][0] - '0');

    /* Parent mode */
    int code;

    /* Test 0: bus error (may not trigger on QEMU) */
    code = run_child_test(0, SIGBUS);
    if (code != 99)
        UT_ASSERT_EQ(code, 128 + SIGBUS);
    /* else: fault didn't trigger — QEMU limitation, skip */

    /* Test 1: illegal instruction */
    code = run_child_test(1, SIGILL);
    UT_ASSERT_EQ(code, 128 + SIGILL);

    /* Test 2: divide by zero */
    code = run_child_test(2, SIGFPE);
    UT_ASSERT_EQ(code, 128 + SIGFPE);

    /* Test 3: odd address (may not trigger on QEMU) */
    code = run_child_test(3, SIGBUS);
    if (code != 99)
        UT_ASSERT_EQ(code, 128 + SIGBUS);

    UT_SUMMARY("test_fault");
}
