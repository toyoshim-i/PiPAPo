/*
 * target_qemu_arm.c — Target implementation for QEMU mps2-an500
 *
 * QEMU ARM: no PLL, no SPI, no SD, no Core 1, CMSDK UART.
 * Uses a RAM-backed block device from an embedded FAT32 image.
 */

#include "target/target.h"
#include "kernel/core/driver/blkdev.h"
#include "kernel/core/driver/ramblk.h"
#include "kernel/core/driver/uart.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/mm/mem_region.h"
#include "kernel/core/mm/page.h"

#ifdef PPAP_TESTS
#include "ktest.h"
#endif

/* Linker-provided FAT32 test image (from fatimg_data.S) */
extern const uint8_t __fatimg_start[];
extern const uint8_t __fatimg_end[];

void target_early_init(void) {
  uart_init();
  mod_vfs.klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
  mod_vfs.klogf("PiPAPo booting... [qemu_arm]\n");
  mod_vfs.klogf("UART: CMSDK UART0 @ 0x40004000\n");
  mod_vfs.klogf("Clock: emulated (no PLL)\n");
  /* No PLL, no SPI */
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

    if (mem_region_alloc(&image_region, PPAP_MEM_RAM_DATA, PAGE_SIZE,
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
