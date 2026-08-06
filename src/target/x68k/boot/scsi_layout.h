/*
 * scsi_layout.h — PPAP X68000 SCSI (.hds) disk layout.
 *
 * The disk is laid out for the ROM SCSIINROM boot path (512-byte records):
 *
 *   record 0  (byte 0x000)  X68SCSI1 device-init header
 *   record 1  (byte 0x200)  unused (zero)
 *   record 2  (byte 0x400)  SCSI IPL loader — SCSIINROM reads 1024 B here to
 *                           0x2000, checks the first byte is $60, and JSRs it
 *   record 4+ (byte 0x800)  rootfs UFS  → LBA SCSI_UFS_BASE
 *
 * Shared by the SCSI boot loader (stage2_scsi.c), the kernel block driver
 * (scsi_blk.c), and the image builder (mkx68kimg.sh greps SCSI_UFS_BASE), so
 * the rootfs base LBA has a single source of truth.
 */
#ifndef PPAP_X68K_SCSI_LAYOUT_H
#define PPAP_X68K_SCSI_LAYOUT_H

/* First LBA of the rootfs UFS on the .hds (512-byte records).  A blkdev
 * sector S maps to LBA (SCSI_UFS_BASE + S). */
#define SCSI_UFS_BASE 4u

#endif /* PPAP_X68K_SCSI_LAYOUT_H */
