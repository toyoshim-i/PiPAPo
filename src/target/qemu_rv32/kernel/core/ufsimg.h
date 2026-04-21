/*
 * ufsimg.h -- Embedded UFS test image symbols (linker-provided)
 *
 * The image bytes are emitted by ufsimg_data.S and placed into the
 * kernel's .rodata section; qemu_rv32.ld sets __ufsimg_{start,end}
 * around them so target_qemu_rv32 can register the image as a
 * flatblk device at late_init.
 */

#ifndef PPAP_TARGET_QEMU_RV32_KERNEL_CORE_UFSIMG_H
#define PPAP_TARGET_QEMU_RV32_KERNEL_CORE_UFSIMG_H

#include <stdint.h>

extern const uint8_t __ufsimg_start[];
extern const uint8_t __ufsimg_end[];

#endif /* PPAP_TARGET_QEMU_RV32_KERNEL_CORE_UFSIMG_H */
