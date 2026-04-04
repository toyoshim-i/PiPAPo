/*
 * target_pico2rv.c — Target implementation for Raspberry Pi Pico 2 (RISC-V)
 *
 * Pico 2: RP2350, 4 MB flash, no SD card, dual-core (Hazard3 RISC-V).
 * No SPI bus — omits spi_init() and sd_init().
 */

#include "target/target.h"
#include "pico2rv.h"
#include "kernel/common/config.h"
#include "kernel/core/driver/uart.h"
#include "kernel/core/driver/uart_rp2350.h"
#include "kernel/core/driver/clock.h"
#include "kernel/common/mod/mod_vfs.h"
#include "target/rpico.h"

#ifdef PPAP_TESTS
#include "ktest.h"
#endif

/* ── RP2350 bootrom state reset ─────────────────────────────────────────── *
 *
 * The Pico SDK calls rom_bootrom_state_reset() as the very first thing in
 * runtime_init.  It resets global hardware state (interrupt controllers,
 * permissions, etc.) left over from the boot ROM or a previous crash.
 * Without this, RISC-V registers (Hazard3 external IRQ arrays, etc.) can
 * contain stale values that cause unpredictable behavior.
 *
 * ROM function lookup for RISC-V:
 *   1. Read the rom_table_lookup function pointer from ROM address 0x7DFA
 *   2. Call it with code='SR' and flag=RT_FLAG_FUNC_RISCV to get state_reset
 *   3. Call state_reset(GLOBAL_STATE | CURRENT_CORE)
 */
static void bootrom_state_reset(void)
{
    typedef void *(*rom_table_lookup_fn)(uint32_t code, uint32_t mask);
    typedef void (*rom_state_reset_fn)(uint32_t flags);

    /* RP2350 RISC-V bootrom stores the table lookup entry at 0x7DFA. */
    rom_table_lookup_fn lookup =
        (rom_table_lookup_fn)(uintptr_t)*(uint16_t *)0x7DFAu;
    rom_state_reset_fn reset_fn =
        (rom_state_reset_fn)lookup('S' | ('R' << 8), 0x0001u);

    /* GLOBAL_STATE only — CURRENT_CORE (0x01) appears to reset Hazard3
     * CPU config (possibly disabling C extension → illegal insn crashes).
     * CURRENT_CORE is auto-called by bootrom during normal boot anyway. */
    reset_fn(0x04u); /* BOOTROM_STATE_RESET_GLOBAL_STATE */
}

/* Clear Hazard3 external IRQ force arrays (MEIFA) — stale forced interrupts
 * from a previous crash could fire once mstatus.MIE is enabled.
 * Equivalent to SDK's runtime_init_per_core_h3_irq_registers, but without
 * enabling global interrupts (we defer that to riscv_timer_init). */
static void h3_clear_irq_force(void)
{
    /* MEIFA CSR 0xBE2: writing index N clears force array word N */
    for (int i = 3; i >= 0; i--)
        __asm__ volatile ("csrw 0xBE2, %0" : : "r"(i));
    /* Clear mie — no interrupts until riscv_timer_init */
    __asm__ volatile ("csrw mie, zero");
    /* Do NOT clear mscratch — it holds the kernel stack pointer for the
     * mscratch-based stack split (trap.S: csrrw sp, mscratch, sp).
     * Zeroing it would cause sp=0 on any exception, leading to an
     * unrecoverable fault loop on real hardware. */
}

void target_early_init(void)
{
    bootrom_state_reset();   /* restore global bootrom state, incl. perms */
    h3_clear_irq_force();   /* clear stale Hazard3 IRQ force bits    */

    /* Release all SIO hardware spinlocks — a previous crash may have left
     * one held.  spin_lock_irqsave() in uart_putc would deadlock otherwise. */
    for (int i = 0; i < 32; i++)
        *(volatile uint32_t *)(0xD0000100u + i * 4u) = 0;

    /* ── SDK-style early resets ──────────────────────────────────────────
     * Reset ALL peripherals to a known state, except:
     *   - IO_QSPI, PADS_QSPI (fatal if running from flash)
     *   - PLL_SYS, PLL_USB   (fatal if clock muxing hasn't been reset)
     *   - USBCTRL, SYSCFG    (disturbs USB-to-SWD on core 1)
     * Then unreset peripherals clocked only by clk_sys/clk_ref.
     * UART0/SPI stay in reset until their drivers init them. */
#define KEEP_MASK (RESETS_RESET_IO_QSPI_BITS | RESETS_RESET_PADS_QSPI_BITS | \
                   RESETS_RESET_PLL_SYS_BITS | RESETS_RESET_PLL_USB_BITS | \
                   RESETS_RESET_SYSCFG_BITS | RESETS_RESET_USBCTRL_BITS)
    {
        uint32_t assert_mask = ~KEEP_MASK & 0x1FFFFFFFu;

        /* Assert reset on everything except KEEP_MASK */
        RESETS_RESET_SET = assert_mask;

        /* Release all asserted resets */
        RESETS_RESET_CLR = assert_mask;

        /* Only wait for peripherals we need before clock init:
         * IO_BANK0, PADS_BANK0.  Other peripherals (UART, DMA,
         * etc.) will be waited on by their respective drivers. */
#define NEED_MASK (RESETS_RESET_IO_BANK0_BITS | RESETS_RESET_PADS_BANK0_BITS)
        while ((RESETS_RESET_DONE & NEED_MASK) != NEED_MASK)
            ;
    }

    uart_init();
    mod_vfs.klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
    /* No bytes have been emitted yet, so there is nothing to drain here.
     * On Hazard3, UART_FR.BUSY can remain set spuriously and hang boot if we
     * spin on uart_tx_drain() before the first write. */
    clock_init_pll();          /* switch clk_sys to PLL (PPAP_SYS_HZ)      */
    uart_reinit_pll();         /* set PLL-speed baud divisors               */
    mod_vfs.klogf("PiPAPo booting... [pico2rv]\n");
    mod_vfs.klogf("System clock: %u MHz\n", PPAP_SYS_HZ / 1000000u);
    /* No SPI init — pico2rv has no SD card slot */
}

/* Timer init — defined in riscv_common.c */
extern void riscv_timer_init(void);

/* UART RX availability check — declared in uart_rp2350.c */
extern int uart_rx_avail(void);

void target_late_init(void)
{
    /* No SD card to initialize */
    while (uart_getc() >= 0) ;   /* drain boot noise from RX ring */
    riscv_timer_init();            /* start 10ms tick timer         */

    /* Register UART RX polling for the serial TTY.
     * RISC-V UART is polled (no RX interrupt), so the idle loop's
     * sched_display_poll() checks uart_rx_avail() every 20ms and
     * wakes blocked tty readers when data arrives. */
    extern void sched_set_input_poll(int (*fn)(void), int tty_idx);
    sched_set_input_poll(uart_rx_avail, 0 /* TTY_SERIAL */);
}

void target_post_mount(void)
{
#ifdef PPAP_TESTS
    ktest_run_all();
#endif
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
    return "pico2rv";
}

uint32_t target_caps(void)
{
    return TARGET_CAP_REALUART;
    /* No TARGET_CAP_CORE1 (single-core initial port), no SD, no SPI */
}

/* ── Debug hardware breakpoints ──────────────────────────────────────────── *
 *
 * Hazard3 supports RISC-V trigger module (tselect/tdata1/tdata2) for
 * hardware breakpoints.  Stubbed for now — Phase RV-5 will implement.
 */

uint32_t target_debug_hwbp_slots(void)
{
    return 0;
}

int target_debug_hwbp_set(uint32_t slot, uint32_t addr)
{
    (void)slot;
    (void)addr;
    return -1;
}

int target_debug_hwbp_clear(uint32_t slot)
{
    (void)slot;
    return -1;
}
