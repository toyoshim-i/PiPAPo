/*
 * test_exec.c — ELF loading, XIP execution, and user-mode verification
 *
 * If we reach main(), exec + XIP worked.  Verify the execution
 * environment: PC in flash, SP in SRAM, getpid > 0.
 */

#include "utest.h"

int main(void)
{
    /* 1. If we reached here, exec + XIP worked */
    UT_ASSERT(1, "exec reached main");

    /* 2. Verify PC is in expected code range */
    uint32_t pc;
#if defined(__m68k__)
    __asm__ volatile("lea %%pc@(0), %0" : "=a"(pc));
    /* m68k: code is loaded into RAM pages */
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
#else
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    UT_ASSERT(sp >= 0x20000000 && sp < 0x20042000,
              "SP should be in SRAM range");
#endif

    /* 4. Verify getpid works from user mode */
    pid_t pid = getpid();
    UT_ASSERT(pid > 0, "getpid should return positive PID");

    UT_SUMMARY("test_exec");
}
