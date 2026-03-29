/*
 * target_qemu_rv32.c — Target implementation for QEMU RISC-V virt machine
 *
 * QEMU virt: NS16550A UART, CLINT timer, no SD/SPI/display.
 * Used for CI testing and debugging the RISC-V port without hardware.
 *
 * Block device: embedded UFS image (flatblk) for musl-linked test binaries
 * that are too large for romfs.
 */

#include "../target.h"
#include "blkdev/blkdev.h"
#include "blkdev/flatblk.h"
#include "config.h"
#include "drivers/uart.h"
#include "fs/ufs.h"
#include "klog.h"
#include "vfs/vfs.h"

#ifdef PPAP_TESTS
#include "ktest.h"
#endif

/* Linker-provided UFS test image (from ufsimg_data.S) */
extern const uint8_t __ufsimg_start[];
extern const uint8_t __ufsimg_end[];

extern void riscv_timer_init(void);
extern int uart_rx_avail(void);

void target_early_init(void) {
  uart_init();
  klog("PiPAPo booting... [qemu_rv32]\n");
  klogf("System clock: %lu MHz\n", (unsigned long)(PPAP_SYS_HZ / 1000000u));
}

void target_late_init(void) {
  while (uart_getc() >= 0)
    ; /* drain boot noise */
  riscv_timer_init();

  extern void sched_set_input_poll(int (*fn)(void), int tty_idx);
  sched_set_input_poll(uart_rx_avail, 0 /* TTY_SERIAL */);
}

void target_post_mount(void) {
  /* Mount embedded UFS image as /mnt/ufs (musl-linked test binaries) */
  uint32_t ufsimg_size = (uint32_t)(__ufsimg_end - __ufsimg_start);
  if (ufsimg_size >= 512) {
    int rc = flatblk_init("ram0", __ufsimg_start, ufsimg_size);
    if (rc >= 0) {
      blkdev_t *bd = blkdev_find("ram0");
      if (bd) {
        rc = vfs_mount("/mnt/ufs", &ufs_ops, MNT_RDONLY, bd);
        if (rc == 0)
          klogf("VFS: UFS mounted at /mnt/ufs (%lu KB)\n",
                (unsigned long)(ufsimg_size / 1024));
        else
          klogf("VFS: UFS mount failed (%lu)\n", (unsigned long)(uint32_t)(-rc));
      }
    }
  }
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

const char *target_name(void) { return "qemu_rv32"; }

uint32_t target_caps(void) {
  return TARGET_CAP_REALUART;
}

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
