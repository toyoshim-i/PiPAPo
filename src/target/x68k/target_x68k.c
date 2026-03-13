/*
 * target_x68k.c — Target implementation for X68000
 *
 * Implements the target hook API (target.h) for the Sharp X68000 personal
 * computer.  Console I/O is provided by the IPL IOCS via TRAP #15.
 *
 * X68000 hardware:
 *   CPU:     Motorola 68000 @ 10 MHz
 *   RAM:     1 MB standard, up to 12 MB expanded
 *   Console: TVRAM text screen via IOCS _B_PUTC / _B_GETC / _B_KEYSNS
 *   Timer:   MFP (MC68901) Timer-C at 100 Hz (Phase X-2)
 *   Storage: 5-inch floppy (Phase X-3 UFS boot floppy)
 *
 * Boot flow:
 *   Stage1 (sector 0) → Stage2 (sectors 1–3) → kernel at 0x006000
 *   Stage2 copies .vectors to 0x000000, restores TRAP #15 to IPL IOCS,
 *   then jumps to Reset_Handler at 0x006400.
 *
 * NOTE (Phase X-1):
 *   - Cooperative scheduling only (no preemptive timer yet)
 *   - Embedded romfs for initial bring-up (removed in Phase X-3)
 *   - Requires at least 4 MB expanded X68000 RAM for full romfs
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

/* Defined in drivers/timer_x68k.c */
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
 * Maps the m68k register frame onto the shared syscall_dispatch() API.
 * ────────────────────────────────────────────────────────────────────── */

#include "syscall/syscall.h"
#include "signal/signal.h"

void m68k_syscall_entry(uint32_t *regs)
{
    uint32_t nr   = regs[0];       /* d0 = syscall number */
    uint32_t a4   = regs[5];       /* d5 = arg 5 */
    uint32_t a5   = regs[8];       /* a0 = arg 6 */

    uint32_t saved_d1 = regs[1];

    current->svc_needs_restart = 0;

    syscall_dispatch(&regs[1], nr, a4, a5);

    regs[0] = regs[1];

    while (current->svc_needs_restart) {
        current->svc_needs_restart = 0;
        regs[0] = nr;
        regs[1] = saved_d1;
        syscall_dispatch(&regs[1], nr, a4, a5);
        regs[0] = regs[1];
    }

    signal_check(regs);
}

/* ── Target hooks ────────────────────────────────────────────────────── */

void target_early_init(void)
{
    uart_init_console();
    klog("PiPAPo booting... [x68k]\n");
    klog("Console: X68000 IOCS (TVRAM)\n");
    klog("Phase X-1: cooperative scheduling, embedded romfs\n");
}

void target_late_init(void)
{
    /* Initialize the MFP Timer-C (no-op in Phase X-1) */
    timer_init();

    /* Register keyboard polling so blocked TTY reads get woken up */
    sched_set_input_poll(uart_rx_avail, TTY_SERIAL);
}

void target_post_mount(void)
{
    /* User-space init (/sbin/init) is launched by main.c via do_execve() */
}

const char *target_init_path(void)
{
#ifdef PPAP_TESTS
#ifdef PPAP_TESTS_EXTENDED
    return "/bin/runtests_ext";
#else
    return "/bin/runtests";
#endif
#else
    return "/sbin/init";
#endif
}

const char *target_name(void)
{
    return "x68k";
}

uint32_t target_caps(void)
{
    return 0;  /* Phase X-1: no SD, no SPI, no Core 1 */
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
