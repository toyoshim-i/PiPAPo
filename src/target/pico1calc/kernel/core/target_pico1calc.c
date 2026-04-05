/*
 * target_pico1calc.c — Target implementation for ClockworkPi PicoCalc
 *
 * PicoCalc: RP2040, 16 MB flash, SPI0 SD card, dual-core.
 * Full hardware feature set: PLL, SPI, SD, IRQ UART, MPU, Core 1.
 */

#include "target/target.h"
#include "kernel/common/config.h"
#include "kernel/core/driver/clock.h"
#include "kernel/core/driver/i2c.h"
#include "kernel/core/driver/i2c_kbd.h"
#include "kernel/core/driver/lcd_panel.h"
#include "kernel/core/driver/spi.h"
#include "kernel/core/driver/spi_lcd.h"
#include "kernel/core/driver/spi_sd.h"
#include "common/errno.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/mm/mpu.h"
#include "pico1calc.h"

#ifdef PPAP_TESTS
#include "ktest.h"
#endif

void target_early_init(void) {
  mod_vfs.notify(VFS_EVENT_WILL_PLL_CHANGE);
  clock_init_pll();
  mod_vfs.notify(VFS_EVENT_PLL_CHANGED);
  spi_init(400000);
  mod_vfs.klogf("SPI0: initialised at 400 kHz\n");
  /* Probe I2C first to detect PicoCalc carrier board (STM32 keyboard
   * controller).  LCD init is gated on this because PL022 SPI master
   * mode completes transfers even without a slave, so spi_lcd_ok()
   * cannot detect a missing LCD. */
  i2c_init();
  mod_vfs.klogf("I2C1: initialised at 10 kHz\n");
  kbd_init();
  if (kbd_present()) {
    spi_lcd_init();
    mod_vfs.klogf("SPI1: LCD initialised at 33 MHz\n");
    lcd_init();
    if (!spi_lcd_ok())
      mod_vfs.klogf("LCD: *** SPI timeout during init ***\n");
    else
      mod_vfs.klogf("LCD: ST7365P initialised (320x320 RGB565)\n");
  } else {
    mod_vfs.klogf("PicoCalc peripherals not detected (skipping LCD/fbcon)\n");
  }
}

void target_late_init(void) {
  /* TODO: SD init disabled — spi_xfer() hangs on the first SPI0 data
   * transfer after UF2 bootloader warm boot.  The PL022 accepts TX data
   * but never produces RX data, suggesting clk_peri or GPIO mux issue.
   * Investigate: try SPI0 loopback test, verify clk_peri, check if the
   * bootloader's boot2 reconfigures pin mux for QSPI that conflicts. */
#if 0
    int rc = sd_init();
    if (rc == 0)
        mod_vfs.klogf("SD: card initialised, mmcblk0 registered\n");
    else if (rc == -ENODEV)
        mod_vfs.klogf("SD: no card detected (skipping)\n");
    else
        mod_vfs.klogf("SD: init failed (err=%u)\n", (unsigned)(-(int)rc));
#else
  mod_vfs.klogf("SD: disabled (SPI0 hang under investigation)\n");
#endif

  mod_vfs.notify(VFS_EVENT_LATE_INIT);
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

const char *target_name(void) { return "pico1calc"; }

uint32_t target_caps(void) {
  uint32_t caps =
      TARGET_CAP_SD | TARGET_CAP_SPI | TARGET_CAP_CORE1 | TARGET_CAP_REALUART;
  if (kbd_present()) caps |= TARGET_CAP_DISPLAY | TARGET_CAP_KBD;
  return caps;
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
