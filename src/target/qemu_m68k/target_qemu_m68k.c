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
#include "fd/tty.h"
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
 * Maps the m68k register frame onto the shared syscall_dispatch() API:
 *   frame[0..3] = d1..d4 (args 0-3), nr = d0, a4 = d5, a5 = a0.
 * syscall_dispatch() writes the return value into frame[0] (d1 slot);
 * we then copy it to regs[0] (d0) for the caller.
 * ────────────────────────────────────────────────────────────────────── */

#include "syscall/syscall.h"

void m68k_syscall_entry(uint32_t *regs)
{
    uint32_t nr   = regs[0];       /* d0 = syscall number */
    uint32_t a4   = regs[5];       /* d5 = arg 5 */
    uint32_t a5   = regs[8];       /* a0 = arg 6 */

    /* Save original d1 (first arg) for potential restart.
     * These are C locals on the process stack, so they survive context
     * switches (TRAP #1 / timer ISR) — unlike the globals m68k_saved_nr
     * and svc_saved_a0 which leak between processes. */
    uint32_t saved_d1 = regs[1];

    current->svc_needs_restart = 0;

    syscall_dispatch(&regs[1], nr, a4, a5);

    /* syscall_dispatch wrote return value to regs[1] (frame[0]).
     * Move it to d0 (regs[0]) for the Linux m68k ABI. */
    regs[0] = regs[1];

    /* Handle restart: blocking syscalls (read, waitpid, sleep, etc.)
     * set current->svc_needs_restart = 1 before yielding.  When woken,
     * we loop here to re-execute the syscall with original arguments.
     * The per-process flag is safe across context switches, unlike the
     * global svc_restart which gets corrupted by other processes. */
    while (current->svc_needs_restart) {
        current->svc_needs_restart = 0;
        regs[0] = nr;
        regs[1] = saved_d1;
        syscall_dispatch(&regs[1], nr, a4, a5);
        regs[0] = regs[1];
    }
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

    /* Register UART input polling so blocked TTY reads get woken up */
    sched_set_input_poll(uart_rx_avail, TTY_SERIAL);

    /* No IRQ UART, no MPU, no Core 1 on m68k */
}

void target_post_mount(void)
{
    /* User-space init (/sbin/init) is launched by main.c via do_execve() */
}

const char *target_init_path(void)
{
    return "/sbin/init";
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
