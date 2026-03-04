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

    /* 2. Verify we are running from flash (XIP) */
    uint32_t pc;
    __asm__ volatile("mov %0, pc" : "=r"(pc));
    UT_ASSERT(pc >= 0x00000000 && pc < 0x20000000,
              "PC should be in flash/XIP range");

    /* 3. Verify stack is in SRAM */
    uint32_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    UT_ASSERT(sp >= 0x20000000 && sp < 0x20042000,
              "SP should be in SRAM range");

    /* 4. Verify getpid works from user mode */
    pid_t pid = getpid();
    UT_ASSERT(pid > 0, "getpid should return positive PID");

    UT_SUMMARY("test_exec");
}
