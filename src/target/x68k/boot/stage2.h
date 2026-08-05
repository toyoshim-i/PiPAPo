/*
 * stage2.h — Interface between the device-agnostic stage2 core and its
 * per-device block backends.
 *
 * stage2_core.c parses the 44bsd UFS and loads /boot/kernel; it reads the
 * disk only through read_ufs_block(), which stage2_floppy.c (IOCS _B_READ,
 * CHS floppy) and stage2_scsi.c (IOCS _SCSIDRV, SCSI HDD) implement.  Each
 * backend also declares the medium it boots from via stage2_boot_device.
 */
#ifndef PPAP_X68K_STAGE2_H
#define PPAP_X68K_STAGE2_H

#include <stdint.h>

/* 44bsd UFS geometry shared by the core (byte↔fragment math) and the backends
 * (fragment↔device-sector math).  One read_ufs_block() call transfers one
 * UFS_BLOCK_SIZE block. */
#define UFS_BLOCK_SIZE 4096u
#define UFS_FRAG_SIZE 512u

/* Read one UFS block (UFS_BLOCK_SIZE bytes), addressed by its starting
 * fragment number `frag` (a multiple of UFS_BLOCK_SIZE/UFS_FRAG_SIZE), into
 * `dest`.  Implemented by the linked-in device backend. */
void read_ufs_block(uint32_t frag, void *dest);

/* Identity of the medium this stage2 boots from — one of BOOT_DEV_* from
 * boot_handoff.h.  Defined by the linked-in device backend; the core records
 * it in the boot handoff for the kernel. */
extern const uint8_t stage2_boot_device;

#endif /* PPAP_X68K_STAGE2_H */
