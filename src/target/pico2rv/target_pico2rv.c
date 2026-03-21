/*
 * target_pico2rv.c — Target implementation for Raspberry Pi Pico 2 (RISC-V)
 *
 * Pico 2: RP2350, 4 MB flash, no SD card, dual-core (Hazard3 RISC-V).
 * No SPI bus — omits spi_init() and sd_init().
 */

#include "../target.h"
#include "pico2rv.h"
#include "config.h"
#include "drivers/uart.h"
#include "drivers/arch/arm_m/uart_rpico.h"
#include "drivers/clock.h"
#include "klog.h"

#ifdef PPAP_TESTS
#include "ktest.h"
#endif

void target_early_init(void)
{
    uart_init();
    klog("PiPAPo booting... [pico2rv]\n");
    klog("UART: 115200 bps @ 12 MHz XOSC\n");
    uart_tx_drain();           /* drain at 12 MHz; also disables UART0 NVIC */
    clock_init_pll();          /* switch clk_sys to PLL (PPAP_SYS_HZ)      */
    uart_reinit_pll();         /* set PLL-speed baud divisors               */
    klogf("System clock: %u MHz\n", PPAP_SYS_HZ / 1000000u);
    /* No SPI init — pico2rv has no SD card slot */
}

/* Timer init — defined in riscv_common.c */
extern void riscv_timer_init(void);

void target_late_init(void)
{
    /* No SD card to initialize */
    while (uart_getc() >= 0) ;   /* drain boot noise from RX ring */
    riscv_timer_init();            /* start 10ms tick timer         */
    /* PMP init deferred to Phase RV-3 */
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
