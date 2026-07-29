/*
 * scsi_blk.h — X68000 SCSI hard-disk block device ("sd0")
 *
 * Presents a SCSI hard disk as a blkdev_t backed by the IPL ROM's SCSI IOCS
 * (_SCSIDRV, IOCS $F5).  Registered during target bring-up; the kernel mounts
 * its 44bsd UFS as the rootfs.
 */

#ifndef PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_SCSI_BLK_H
#define PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_SCSI_BLK_H

/* Probe SCSI target 0 via IOCS and, if present, register it as "sd0".
 * Call once during target_late_init(), after the IOCS guard is up. */
void scsi_blk_init(void);

#endif /* PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_SCSI_BLK_H */
