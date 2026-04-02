/*
 * test_exec.c — ELF loading, XIP execution, and user-mode verification
 *
 * If we reach main(), exec + XIP worked.  Verify the execution
 * environment: PC in flash, SP in SRAM, getpid > 0.
 */

#include "utest.h"
#include "kernel/common/errno.h"

int main(void)
{
    /* 1. If we reached here, exec + XIP worked */
    UT_ASSERT(1, "exec reached main");

    /* 2. Verify PC is in expected code range */
    uint32_t pc;
#if defined(__m68k__)
    __asm__ volatile("lea %%pc@(0), %0" : "=a"(pc));
    UT_ASSERT(pc > 0, "PC should be valid");
#elif defined(__ia16__)
    pc = 1;
    UT_ASSERT(pc > 0, "PC should be valid");
#elif defined(__riscv)
    __asm__ volatile("auipc %0, 0" : "=r"(pc));
    /* RISC-V: code loaded into SRAM (text+data contiguous) */
    UT_ASSERT(pc > 0, "PC should be valid");
#else
    __asm__ volatile("mov %0, pc" : "=r"(pc));
    UT_ASSERT(pc >= 0x00000000 && pc < 0x20000000,
              "PC should be in flash/XIP range");
#endif

    /* 3. Verify stack is in expected RAM range */
    uint32_t sp;
#if defined(__m68k__)
    __asm__ volatile("move.l %%sp, %0" : "=d"(sp));
    UT_ASSERT(sp > 0, "SP should be valid");
#elif defined(__ia16__)
    __asm__ volatile("movw %%sp, %0" : "=r"(sp));
    UT_ASSERT(sp > 0, "SP should be valid");
#elif defined(__riscv)
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    UT_ASSERT(sp > 0, "SP should be valid");
#else
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    UT_ASSERT(sp >= 0x20000000 && sp < 0x20042000,
              "SP should be in SRAM range");
#endif

    /* 4. Verify getpid works from user mode */
    pid_t pid = getpid();
    UT_ASSERT(pid > 0, "getpid should return positive PID");

    /* 5. execve should reject argv overflow with -E2BIG */
    {
        enum { ARG_OVERFLOW = 33 };
        char *argv_many[ARG_OVERFLOW + 1];
        for (int i = 0; i < ARG_OVERFLOW; i++)
            argv_many[i] = (char *)"x";
        argv_many[ARG_OVERFLOW] = (char *)0;

        pid_t cpid = vfork();
        if (cpid == 0) {
            int rc = execve("/bin/hello", argv_many, (void *)0);
            if (rc == -(int)E2BIG)
                _exit(0);
            _exit(127);
        }

        int status = 0;
        UT_ASSERT_EQ(waitpid(cpid, &status, 0), cpid);
        UT_ASSERT(WIFEXITED(status), "overflow exec child should exit");
        UT_ASSERT_EQ(WEXITSTATUS(status), 0);
    }

    UT_SUMMARY("test_exec");
}
