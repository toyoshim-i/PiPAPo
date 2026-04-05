/*
 * target_qemu_rv32.c — Target implementation for QEMU RISC-V virt machine
 *
 * QEMU virt: NS16550A UART, CLINT timer, no SD/SPI/display.
 * Used for CI testing and debugging the RISC-V port without hardware.
 *
 * Block device: embedded UFS image (flatblk) for musl-linked test binaries
 * that are too large for romfs.
 */

#include "target/target.h"
#include "kernel/vfs/driver/blkdev.h"
#include "kernel/vfs/driver/flatblk.h"
#include "kernel/common/mod/mod_vfs.h"

#ifdef PPAP_TESTS
#include "ktest.h"
#endif

/* Linker-provided UFS test image (from ufsimg_data.S) */
extern const uint8_t __ufsimg_start[];
extern const uint8_t __ufsimg_end[];

extern void riscv_timer_init(void);

void target_early_init(void) {
  /* Boot banner printed from klog_init_logger() (VFS side) */
}

void target_late_init(void) {
  riscv_timer_init();
  mod_vfs.notify(VFS_EVENT_LATE_INIT);
}

void target_post_mount(void) {
  /* Mount embedded UFS image as /mnt/ufs (musl-linked test binaries) */
  uint32_t ufsimg_size = (uint32_t)(__ufsimg_end - __ufsimg_start);
  if (ufsimg_size >= 512) {
    int rc = flatblk_init("ram0", __ufsimg_start, ufsimg_size);
    if (rc >= 0) {
      blkdev_t *bd = blkdev_find("ram0");
      if (bd) {
        rc = mod_vfs.mount_ufs("/mnt/ufs", MNT_RDONLY, bd);
        if (rc == 0)
          mod_vfs.klogf("VFS: UFS mounted at /mnt/ufs (%lu KB)\n",
                (unsigned long)(ufsimg_size / 1024));
        else
          mod_vfs.klogf("VFS: UFS mount failed (%lu)\n", (unsigned long)(uint32_t)(-rc));
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
