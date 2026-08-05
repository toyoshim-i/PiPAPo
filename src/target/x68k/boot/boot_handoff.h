/*
 * boot_handoff.h — Fixed low-RAM handoff between the X68000 boot loader and
 * the kernel.
 *
 * The boot loader writes these words just before jumping to the kernel; the
 * kernel reads them in target_early_init() — before target_late_init()
 * reclaims 0x000400-0x005FFF as page-pool memory and before the supervisor
 * stack (growing down from 0x006400) can reach them.
 *
 * Shared verbatim by the boot stages (stage1/stage2, compiled as .S/.c with
 * the C preprocessor) and the kernel (target_x68k.c), so the addresses,
 * magic, and device codes have a single source of truth.
 */
#ifndef PPAP_X68K_BOOT_HANDOFF_H
#define PPAP_X68K_BOOT_HANDOFF_H

/* stage1 saves the IPL IOCS (TRAP #15) vector here; stage2_final() restores
 * it into the kernel vector table after the vector copy. */
#define BOOT_HANDOFF_IOCS_SAVE 0x002FF0u

/* Boot-device record.  Each stage2 backend knows the medium it booted from
 * (floppy vs SCSI are distinct code paths) and records its own identity here;
 * the kernel mounts the matching rootfs.  The magic guards against stale RAM
 * so a kernel started by any other means falls back to the floppy default. */
#define BOOT_HANDOFF_MAGIC_ADDR 0x002FF4u
#define BOOT_HANDOFF_DEV_ADDR 0x002FF8u
#define BOOT_HANDOFF_MAGIC 0x50504244u /* 'PPBD' */

#define BOOT_DEV_FLOPPY 0u
#define BOOT_DEV_SCSI 1u

#endif /* PPAP_X68K_BOOT_HANDOFF_H */
