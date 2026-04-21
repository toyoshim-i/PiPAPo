/*
 * fatimg.h -- Embedded FAT test image symbols (linker-provided)
 *
 * The image bytes are emitted by fatimg_data.S and placed into the
 * kernel's .rodata section; qemu.ld sets __fatimg_{start,end}
 * around them so target_qemu_arm can register the image as a ramblk
 * device at late_init.  If no FAT image is bundled the symbols bracket
 * a zero-length region and the register call is skipped.
 */

#ifndef PPAP_TARGET_QEMU_ARM_KERNEL_CORE_FATIMG_H
#define PPAP_TARGET_QEMU_ARM_KERNEL_CORE_FATIMG_H

#include <stdint.h>

extern const uint8_t __fatimg_start[];
extern const uint8_t __fatimg_end[];

#endif /* PPAP_TARGET_QEMU_ARM_KERNEL_CORE_FATIMG_H */
