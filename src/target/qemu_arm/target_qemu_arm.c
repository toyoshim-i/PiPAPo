/*
 * target_qemu_arm.c — Target implementation for QEMU mps2-an500
 *
 * QEMU ARM: no PLL, no SPI, no SD, no Core 1, CMSDK UART.
 * Uses a RAM-backed block device from an embedded FAT32 image.
 */

#include "../target.h"
#include "drivers/uart.h"
#include "klog.h"
#include "mm/page.h"
#include "blkdev/blkdev.h"
#include "blkdev/ramblk.h"

#ifdef PPAP_TESTS
#include "ktest.h"
#endif

/* Linker-provided FAT32 test image (from fatimg_data.S) */
extern const uint8_t __fatimg_start[];
extern const uint8_t __fatimg_end[];

void target_early_init(void)
{
    uart_init_console();
    klog("PicoPiAndPortable booting... [qemu_arm]\n");
    klog("UART: CMSDK UART0 @ 0x40004000\n");
    klog("Clock: emulated (no PLL)\n");
    /* No PLL, no SPI */
}

void target_late_init(void)
{
    /* Register RAM-backed block device from embedded FAT32 image */
    uint32_t fatimg_size = (uint32_t)(__fatimg_end - __fatimg_start);
    if (fatimg_size >= BLKDEV_SECTOR_SIZE) {
        int rc = ramblk_init(__fatimg_start, fatimg_size);
        if (rc >= 0)
            klogf("BLKDEV: ramblk mmcblk0 (FAT32 image, %u KB)\n",
                  fatimg_size / 1024);
        else
            klog("BLKDEV: ramblk init FAILED\n");
    } else {
        /* No FAT32 image — use test pattern (4 KB = 8 sectors) */
        uint8_t *test_img = (uint8_t *)page_alloc();
        if (test_img) {
            __builtin_memset(test_img, 0, PAGE_SIZE);
            __builtin_memset(test_img, 0xAA, BLKDEV_SECTOR_SIZE);
            int rc = ramblk_init(test_img, PAGE_SIZE);
            if (rc >= 0)
                klog("BLKDEV: ramblk mmcblk0 (test, 8 sectors)\n");
            else
                klog("BLKDEV: ramblk init FAILED\n");
        } else {
            klog("BLKDEV: page_alloc failed\n");
        }
    }
    /* Enable UART RX interrupts so tty_rx_notify() wakes blocked readers */
    uart_init_irq();
    /* No MPU, no Core 1 on QEMU */
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
    return "/bin/runtests";
#else
    return "/sbin/init";
#endif
}

uint32_t target_caps(void)
{
    return 0;  /* No SD, no SPI, no Core 1, no PL011 */
}
