/*
 * target_pico2.c — Target implementation for official Raspberry Pi Pico 2
 *
 * Pico 2: RP2350, 4 MB flash, no SD card, dual-core (Cortex-M33).
 * No SPI bus — omits spi_init() and sd_init().
 */

#include "target/target.h"
#include "kernel/common/config.h"
#include "kernel/core/driver/uart_rpico.h"
#include "kernel/core/driver/clock.h"
#include "kernel/core/driver/uart.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/mm/mpu.h"
#include "pico2.h"

#ifdef PPAP_TESTS
#include "ktest.h"
#endif

void target_early_init(void) {
  uart_init();
  mod_vfs.klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
  mod_vfs.klogf("PiPAPo booting... [pico2]\n");
#ifdef PPAP_SEMIHOST
  clock_init_pll(); /* still need PLL for SysTick */
#else
  mod_vfs.klogf("UART: 115200 bps @ 12 MHz XOSC\n");
  uart_tx_drain();   /* drain at 12 MHz; also disables UART0 NVIC */
  clock_init_pll();  /* switch clk_sys to PLL (PPAP_SYS_HZ)      */
  uart_reinit_pll(); /* set PLL-speed baud divisors               */
#endif
  mod_vfs.klogf("System clock: %u MHz\n", PPAP_SYS_HZ / 1000000u);
  /* No SPI init — pico2 has no SD card slot */
}

void target_late_init(void) {
  /* No SD card to initialize */
  while (uart_getc() >= 0)
    ; /* drain boot noise from RX ring */
  mpu_init();
}

void target_post_mount(void) {
#ifdef PPAP_TESTS
  ktest_run_all();
#endif
}

const char *target_init_path(void) {
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

const char *target_name(void) { return "pico2"; }

uint32_t target_caps(void) {
  return TARGET_CAP_CORE1 | TARGET_CAP_REALUART;
  /* No TARGET_CAP_SD, no TARGET_CAP_SPI */
}

/* ── ARM FPB hardware breakpoints (native ptrace backend) ───────────────── *
 *
 * Cortex-M33 (ARMv8-M) uses FPB v2 with a completely different comparator
 * encoding than Cortex-M0+ (ARMv6-M, FPB v1).  The REPLACE field at
 * bits [31:30] is gone; address bits and a MATCH field at [2:1] replace it.
 * Implementing FPB v2 properly is future work — for now, report 0 slots
 * so the ptrace layer hides the HW_BP capability on this target.
 */

uint32_t target_debug_hwbp_slots(void) { return 0; }

int target_debug_hwbp_set(uint32_t slot, uint32_t addr) {
  (void)slot;
  (void)addr;
  return -1;
}

int target_debug_hwbp_clear(uint32_t slot) {
  (void)slot;
  return -1;
}
