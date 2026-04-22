/*
 * target_qemu_rv32.c — Target implementation for QEMU RISC-V virt machine
 *
 * QEMU virt: NS16550A UART, CLINT timer, no SD/SPI/display.
 * Used for CI testing and debugging the RISC-V port without hardware.
 *
 * Block device: embedded UFS image (flatblk) for musl-linked test binaries
 * that are too large for romfs.
 */

#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/timer.h"
#include "kernel/core/ufsimg.h"
// TODO: core-side code including VFS driver headers directly bypasses
// the module bridge.  Non-ia16 target, so no link-time concern today;
// switch to a mod_vfs.* path if one becomes available without having
// to promote flatblk_init / blkdev_find into the mod_vfs vtable (same
// cleanup pending in target_x68k.c).
#include "kernel/vfs/driver/blkdev.h"
#include "kernel/vfs/driver/flatblk.h"
#include "target/target.h"

#ifdef PPAP_TESTS
#include "ktest.h"
#endif

/* sifive_test device — built into QEMU virt rv32 */
#define SIFIVE_TEST_BASE 0x100000u
#define SIFIVE_TEST_PASS 0x5555u
#define SIFIVE_TEST_FAIL 0x3333u

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
  /* TODO: kernel tests crash rv32 (blkdev tests expect FAT32 ramblk) */
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

/*
 * QEMU poweroff via sifive_test device.
 *
 * Writing 0x5555 triggers FINISHER_PASS (QEMU exits 0).
 * Writing 0x3333 triggers FINISHER_FAIL (QEMU exits 1).
 */
void target_qemu_poweroff(uint8_t status) {
  volatile uint32_t *test = (volatile uint32_t *)SIFIVE_TEST_BASE;
  *test = status ? SIFIVE_TEST_FAIL : SIFIVE_TEST_PASS;
  for (;;) __asm__ volatile("nop");
}
