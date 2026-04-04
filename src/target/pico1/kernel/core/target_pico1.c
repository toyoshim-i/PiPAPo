/*
 * target_pico1.c — Target implementation for official Raspberry Pi Pico
 *
 * Pico 1: RP2040, 2 MB flash, no SD card, dual-core.
 * No SPI bus — omits spi_init() and sd_init().
 */

#include "target/target.h"
#include "kernel/common/config.h"
#include "kernel/core/driver/uart_rpico.h"
#include "kernel/core/driver/clock.h"
#include "kernel/core/driver/uart.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/mm/mpu.h"
#include "pico1.h"

#ifdef PPAP_TESTS
#include "ktest.h"
#endif

void target_early_init(void) {
  uart_init();
  mod_vfs.klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
  mod_vfs.klogf("PiPAPo booting... [pico1]\n");
  mod_vfs.klogf("UART: 115200 bps @ 12 MHz XOSC\n");
  uart_tx_drain();   /* drain at 12 MHz; also disables UART0 NVIC */
  clock_init_pll();  /* switch clk_sys to PLL (PPAP_SYS_HZ)      */
  uart_reinit_pll(); /* set PLL-speed baud divisors               */
  mod_vfs.klogf("System clock: %u MHz\n", PPAP_SYS_HZ / 1000000u);
  /* No SPI init — pico1 has no SD card slot */
}

void target_late_init(void) {
  /* No SD card to initialize */
  while (uart_getc() >= 0)
    ; /* drain boot noise from RX ring */
  mpu_init();
  /* core1_launch moved to kmain — must run after init gets PID 1 */
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

const char *target_name(void) { return "pico1"; }

uint32_t target_caps(void) {
  return TARGET_CAP_CORE1 | TARGET_CAP_REALUART;
  /* No TARGET_CAP_SD, no TARGET_CAP_SPI */
}

/* ── ARM FPB hardware breakpoints (native ptrace backend) ───────────────── */

#define ARM_DEMCR_ADDR (0xE000EDFCu)
#define ARM_FPB_BASE (0xE0002000u)
#define ARM_FPB_CTRL (*(volatile uint32_t *)(ARM_FPB_BASE + 0x00u))
#define ARM_FPB_COMP(i) \
  (*(volatile uint32_t *)(ARM_FPB_BASE + 0x08u + ((i)*4u)))
#define ARM_DEMCR (*(volatile uint32_t *)ARM_DEMCR_ADDR)

#define ARM_DEMCR_TRCENA (1u << 24)
#define ARM_FPB_CTRL_ENABLE (1u << 0)

static uint32_t arm_fpb_code_slots(void) {
  uint32_t ctrl = ARM_FPB_CTRL;
  uint32_t n = ((ctrl >> 4) & 0xFu) | (((ctrl >> 12) & 0x7u) << 4);

  if (n > 4u) n = 4u;
  return n;
}

uint32_t target_debug_hwbp_slots(void) { return arm_fpb_code_slots(); }

int target_debug_hwbp_set(uint32_t slot, uint32_t addr) {
  uint32_t slots = arm_fpb_code_slots();
  uint32_t replace;
  uint32_t comp;

  if (slot >= slots) return -1;

  ARM_DEMCR |= ARM_DEMCR_TRCENA;
  ARM_FPB_CTRL |= ARM_FPB_CTRL_ENABLE;

  replace = (addr & 0x2u) ? 2u : 1u;
  comp = (addr & 0x1FFFFFFCu) | (replace << 30) | 1u;
  ARM_FPB_COMP(slot) = comp;
  return 0;
}

int target_debug_hwbp_clear(uint32_t slot) {
  uint32_t slots = arm_fpb_code_slots();

  if (slot >= slots) return -1;

  ARM_FPB_COMP(slot) = 0;
  return 0;
}
