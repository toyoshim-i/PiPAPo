/*
 * stage2_scsi.c — Stage 2 SCSI block backend (IOCS _SCSIDRV _S_READ)
 *
 * Reads the 44bsd UFS that begins at LBA SCSI_UFS_BASE on the .hds through
 * the IPL ROM's SCSI BIOS (IOCS _SCSIDRV, $F5), the same call the kernel's
 * scsi_blk driver uses at runtime.  Provides read_ufs_block() for
 * stage2_core.c.  Runs in the single-threaded boot context with IOCS live,
 * so no IOCS guard is needed (unlike the kernel driver).
 */

#include <stdint.h>

#include "boot_handoff.h"
#include "scsi_layout.h"
#include "stage2.h"

/* IOCS _SCSIDRV ($F5) with _S_READ ($21); 512-byte records (block-size code
 * 1) match the .hds bytesPerRecord and scsi_blk. */
#define IOCS_SCSIDRV 0xF5u
#define SCSI_S_READ 0x21u
#define SCSI_BLKSIZE_CODE 1u

/* sc0 — the boot chain targets SCSI ID 0 (matches -boot=sc0 and scsi_blk).
 * The ROM passes the actual boot ID in D4; multi-ID boot is future work. */
#define SCSI_TARGET_ID 0u

const uint8_t stage2_boot_device = BOOT_DEV_SCSI;

/* Read `nblocks` 512-byte records starting at `lba` into `dest`. */
static void read_scsi_blocks(uint32_t lba, uint32_t nblocks, void *dest) {
  register uint32_t d0 asm("d0") = IOCS_SCSIDRV;
  register uint32_t d1 asm("d1") = SCSI_S_READ;
  register uint32_t d2 asm("d2") = lba;
  register uint32_t d3 asm("d3") = nblocks;
  register uint32_t d4 asm("d4") = SCSI_TARGET_ID;
  register uint32_t d5 asm("d5") = SCSI_BLKSIZE_CODE;
  register void *a1 asm("a1") = dest;
  asm volatile("trap #15"
               : "+r"(d0), "+r"(d1), "+r"(d2), "+r"(d3), "+r"(d4), "+r"(d5),
                 "+r"(a1)
               :
               : "a0", "memory");
  (void)d0; /* the ROM already booted us — mirror the floppy read path */
}

/* Read one 4 KB UFS block, addressed by its starting fragment number. */
void read_ufs_block(uint32_t frag, void *dest) {
  /* Fragment N starts at UFS byte N*512.  The UFS begins at LBA
   * SCSI_UFS_BASE (512-byte records), so UFS byte N*512 → LBA
   * (SCSI_UFS_BASE + N).  One UFS block spans UFS_BLOCK_SIZE/512 records. */
  read_scsi_blocks(SCSI_UFS_BASE + frag, UFS_BLOCK_SIZE / UFS_FRAG_SIZE, dest);
}
