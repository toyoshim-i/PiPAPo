/*
 * target_qemu_arm.c — Target implementation for QEMU mps2-an500
 *
 * QEMU ARM: no PLL, no SPI, no SD, no Core 1, CMSDK UART.
 * Uses a RAM-backed block device from an embedded FAT32 image.
 */

#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/exec/image_alloc.h"
#include "kernel/core/fatimg.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/mm/page.h"
// TODO: core-side code including VFS driver headers directly bypasses
// the module bridge.  Non-ia16 target, so no link-time concern today.
// BLKDEV_SECTOR_SIZE already lives in kernel/common/config.h and could
// be pulled from there to drop blkdev.h; ramblk_init is VFS-only and
// would need a mod_vfs entry (out of scope unless another caller
// appears).
#include "kernel/vfs/driver/blkdev.h"
#include "kernel/vfs/driver/ramblk.h"
#include "target/target.h"

#ifdef PPAP_TESTS
#include "ktest.h"
#endif

void target_early_init(void) {
  /* Boot banner printed from klog_init_logger() (VFS side) */
}

void target_late_init(void) {
  /* Register RAM-backed block device from embedded FAT32 image */
  uint32_t fatimg_size = (uint32_t)(__fatimg_end - __fatimg_start);
  if (fatimg_size >= BLKDEV_SECTOR_SIZE) {
    int rc = ramblk_init(__fatimg_start, fatimg_size);
    if (rc >= 0)
      mod_vfs.klogf("BLKDEV: ramblk mmcblk0 (FAT32 image, %u KB)\n",
            fatimg_size / 1024);
    else
      mod_vfs.klogf("BLKDEV: ramblk init FAILED\n");
  } else {
    /* No FAT32 image — use test pattern (4 KB = 8 sectors) */
    proc_image_segment_t image_region;
    uint8_t *test_img = NULL;

    if (image_segment_alloc(&image_region, PPAP_MEM_RAM_DATA, PAGE_SIZE,
                         PROC_IMAGE_SEG_OWNED | PROC_IMAGE_SEG_WRITABLE) == 0)
      test_img = (uint8_t *)image_region.base;
    if (test_img) {
      __builtin_memset(test_img, 0, PAGE_SIZE);
      __builtin_memset(test_img, 0xAA, BLKDEV_SECTOR_SIZE);
      int rc = ramblk_init(test_img, PAGE_SIZE);
      if (rc >= 0)
        mod_vfs.klogf("BLKDEV: ramblk mmcblk0 (test, 8 sectors)\n");
      else
        mod_vfs.klogf("BLKDEV: ramblk init FAILED\n");
    } else {
      mod_vfs.klogf("BLKDEV: test image alloc failed\n");
    }
  }
  /* No MPU, no Core 1 on QEMU */
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

const char *target_name(void) { return "qemu_arm"; }

uint32_t target_caps(void) {
  return 0; /* No SD, no SPI, no Core 1, no PL011 */
}

/*
 * QEMU poweroff via ARM semihosting.
 *
 * angel_SWI 0x18 (SYS_EXIT_EXTENDED) tells QEMU to exit.
 * The parameter block contains {reason, subcode}:
 *   reason=0x20026 (ADP_Stopped_ApplicationExit), subcode=exit_code.
 */
void target_may_poweroff(uint8_t status) {
  uint32_t params[2];
  params[0] = 0x20026u; /* ADP_Stopped_ApplicationExit */
  params[1] = (uint32_t)status;
  register uint32_t r0 __asm__("r0") = 0x20u; /* SYS_EXIT_EXTENDED */
  register uint32_t r1 __asm__("r1") = (uint32_t)params;
  __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
  for (;;) __asm__ volatile("" ::: "memory");
}
