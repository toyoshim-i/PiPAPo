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
 * NOTE (Phase X-2):
 *   - Preemptive scheduling via MFP Timer-C at 100 Hz
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
    klog("Phase X-2: preemptive scheduling (MFP Timer-C), embedded romfs\n");
}

/* Benign IRQ handler — silences unhandled hardware IRQs with a bare rte */
extern void m68k_irq_ignore(void);

void target_late_init(void)
{
    /* Initialize MFP Timer-C at 100 Hz for preemptive scheduling */
    timer_init();

    /* Install a benign rte handler for X68000 hardware autovectors (levels
     * 1–6).  Without this, VSYNC/OPM/FDC/DMA interrupts would hit
     * Default_Handler which halts the CPU with stop #0x2700. */
    /* Vector table lives at physical address 0x000000 on 68000.
     * The compiler warns about NULL dereference; suppress it — this is
     * intentional hardware vector table access, not a bug. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    volatile uint32_t *vt = (volatile uint32_t *)0x0;
    uint32_t ignore = (uint32_t)(uintptr_t)m68k_irq_ignore;
    vt[25] = ignore;  /* Level 1: OPM (YM2151) */
    vt[26] = ignore;  /* Level 2: MFP autovector (fallback) */
    vt[27] = ignore;  /* Level 3: reserved */
    vt[28] = ignore;  /* Level 4: SCC / VSYNC */
    vt[29] = ignore;  /* Level 5: FDC */
    vt[30] = ignore;  /* Level 6: DMA */
    /* Level 7 (NMI, vector 31): keep Default_Handler */

    /* MFP (MC68901) uses VECTORED interrupts with VR base set by the IPL
     * BIOS to 0x40 (vector 64).  Sources occupy vectors 64–79.
     * Patch them all to m68k_irq_ignore so MFP timer / keyboard IRQs
     * that fire after arch_irq_enable() don't hit Default_Handler. */
    for (uint32_t v = 64u; v < 80u; v++)
        vt[v] = ignore;
#pragma GCC diagnostic pop

    /* Set up dual-TTY: TTY_DISPLAY = TVRAM (primary), TTY_SERIAL = RS-232C */
    extern void uart_serial_putc(char c);
    static const tty_backend_t tvram_be = {
        .putc     = uart_putc,
        .flush    = NULL,
        .getc     = uart_getc,
        .rx_avail = uart_rx_avail,
        .get_cols = NULL,
        .get_rows = NULL,
    };
    static const tty_backend_t serial_be = {
        .putc     = uart_serial_putc,
        .flush    = NULL,
        .getc     = NULL,
        .rx_avail = NULL,
        .get_cols = NULL,
        .get_rows = NULL,
    };
    tty_set_backend(TTY_DISPLAY, &tvram_be);
    tty_set_backend(TTY_SERIAL,  &serial_be);
    tty_set_console(TTY_DISPLAY);
    klog_set_mirror(uart_serial_putc, NULL);

    /* Register keyboard polling so blocked TTY reads get woken up */
    sched_set_input_poll(uart_rx_avail, TTY_DISPLAY);
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
    return 0;  /* Phase X-2: no SD, no SPI, no Core 1 */
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
