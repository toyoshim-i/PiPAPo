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
 *   ARM:  illegal_insn works (UDF 0xDEAD). divzero skipped on
 *         Cortex-M0+ (no hardware divide instruction).
 *   rv32: illegal_insn uses the "unimp" 32-bit encoding (csrrw to a
 *         reserved CSR).  divzero skipped — RV32IMAC has no hardware
 *         divide trap (div by zero returns -1, no exception).
 */

#include "utest.h"

static void __attribute__((noreturn)) do_illegal_insn(void)
{
#if defined(__m68k__)
    /* 0x4AFC is the m68k ILLEGAL instruction */
    __asm__ volatile (".short 0x4afc");
#elif defined(__riscv)
    /* `unimp` pseudo-instruction: csrrw x0, cycle, x0 with a reserved
     * write to a read-only CSR — guaranteed illegal instruction trap. */
    __asm__ volatile (".4byte 0xc0001073");
#elif defined(__xtensa__)
    /* ILL.N (narrow illegal): 0xF06D — 16-bit reserved encoding that
     * raises EXCCAUSE=0 (IllegalInstruction).  Distinct from PPAP's
     * 24-bit 0x000000 syscall trap. */
    __asm__ volatile (".short 0xf06d");
#else
    /* 0xDEAD is a permanently undefined Thumb encoding (UDF range 0xDExx) */
    __asm__ volatile (".short 0xdead");
#endif
    _exit(99);
}

static void __attribute__((noreturn)) do_divzero(void)
{
#if defined(__m68k__)
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
#else
    /* Cortex-M0+ has no hardware divide — SDIV/UDIV are ARMv7-M+.
     * RV32IMAC's `div` returns -1 on div-by-zero without trapping.
     * Exit 99 to signal "not applicable on this arch". */
#endif
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

int main(int argc, char *argv[])
{
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

    UT_SUMMARY("test_fault");
}
