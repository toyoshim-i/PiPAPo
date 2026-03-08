/*
 * target_qemu_m68k.c — Target implementation for QEMU virt m68k
 *
 * Implements the target hook API (target.h) for the QEMU virt m68k machine.
 * QEMU virt m68k: RAM at 0x0, Goldfish TTY at 0xFF008000, Goldfish RTC
 * timer at 0xFF006000.
 *
 * Run with:
 *   qemu-system-m68k -machine virt -cpu m68000 -nographic \
 *       -kernel ppap_qemu_m68k.elf
 */

#include "../target.h"
#include "drivers/uart.h"
#include "mm/page.h"
#include "proc/proc.h"
#include "proc/sched.h"
#include "klog.h"
#include "arch/arch.h"
#include "errno.h"
#include <stdint.h>
#include <stddef.h>

/* ── Timer driver ────────────────────────────────────────────────────── */

/* Defined in drivers/timer_qemu_m68k.c */
extern void timer_init(void);

/* ── TRAP #0 syscall dispatch ────────────────────────────────────────── *
 *
 * Called from m68k_trap0_handler (trap.S) with a pointer to the saved
 * register frame.  Register layout (Linux m68k / musl convention):
 *
 *   regs[0]  = d0  (syscall number → overwritten with return value)
 *   regs[1]  = d1  (arg 1)
 *   regs[2]  = d2  (arg 2)
 *   regs[3]  = d3  (arg 3)
 *   regs[4]  = d4  (arg 4)
 *   regs[5]  = d5  (arg 5)
 *   regs[8]  = a0  (arg 6)
 *
 * Syscall numbers match Linux m68k (same as i386 for common calls).
 * Phase C implements: exit(1), write(4).  Phase D adds the rest.
 * ────────────────────────────────────────────────────────────────────── */

/* Linux m68k syscall numbers (matching musl) */
#define M68K_SYS_EXIT   1
#define M68K_SYS_WRITE  4

void m68k_syscall_entry(uint32_t *regs)
{
    uint32_t nr  = regs[0];
    long a0 = (long)regs[1];
    long a1 = (long)regs[2];
    long a2 = (long)regs[3];
    long ret;

    switch (nr) {

    case M68K_SYS_EXIT:
        /* exit(status) — mark process free and yield forever */
        current->state = PROC_FREE;
        for (;;)
            sched_yield();
        /* not reached */
        ret = 0;
        break;

    case M68K_SYS_WRITE: {
        /* write(fd, buf, count) → d1=fd, d2=buf, d3=count */
        int fd             = (int)a0;
        const char *buf    = (const char *)(uintptr_t)a1;
        uint32_t count     = (uint32_t)a2;
        if (fd == 1 || fd == 2) {
            for (uint32_t i = 0; i < count; i++)
                uart_putc(buf[i]);
            ret = (long)count;
        } else {
            ret = -(long)EBADF;
        }
        break;
    }

    default:
        ret = -(long)ENOSYS;
        break;
    }

    regs[0] = (uint32_t)ret;
}

/* ── Target hooks ────────────────────────────────────────────────────── */

void target_early_init(void)
{
    uart_init_console();
    klog("PicoPiAndPortable booting... [qemu_m68k]\n");
    klog("UART: Goldfish TTY @ 0xFF008000\n");
    klog("Clock: emulated (no PLL)\n");
    /* romfs is pre-built by mkromfs -b and linked via .incbin */
}

void target_late_init(void)
{
    /* Initialize the Goldfish RTC timer (10 ms periodic) */
    timer_init();
    /* No IRQ UART, no MPU, no Core 1 on m68k */
}

void target_post_mount(void)
{
    /* User-space init (/bin/hello) is launched by main.c via do_execve() */
}

const char *target_init_path(void)
{
    return "/bin/hello";
}

uint32_t target_caps(void)
{
    return 0;  /* No SD, no SPI, no Core 1 */
}

/* Yield-test process — runs on its own stack, yields back to thread 0 */
void yield_test_process(void)
{
    for (int i = 0; i < 4; i++)
        sched_yield();

    current->state = PROC_FREE;
    for (;;)
        sched_yield();
}
