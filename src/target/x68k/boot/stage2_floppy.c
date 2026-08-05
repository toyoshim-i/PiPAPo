/*
 * stage2_floppy.c — Stage 2 floppy block backend (IOCS _B_READ, CHS)
 *
 * Reads the 44bsd UFS that begins at floppy sector 4 (sectors 0=stage1,
 * 1-3=stage2) through the IPL ROM's _B_READ, translating each UFS block to
 * the X68000 2HD CHS geometry.  Provides read_ufs_block() for stage2_core.c.
 */

#include <stdint.h>

#include "boot_handoff.h"
#include "stage2.h"

/* ── X68000 2HD floppy geometry ──────────────────────────────────────────── */

#define FDC_PDA 0x90u    /* 2HD FDD0 */
#define FDC_MODE 0x70u   /* MFM | retry | seek */
#define SEC_LEN_CODE 3u  /* sector length code 3 = 1024 bytes */
#define FLOPPY_SEC 1024u /* bytes per floppy sector */
#define SECS_PER_CYL 16u /* 2 heads × 8 sectors */

/* The 44bsd UFS starts at floppy sector 4 (sectors 0=stage1, 1-3=stage2). */
#define UFS_FLOPPY_BASE 4u
#define UFS_FLOPPY_SECS \
  (UFS_BLOCK_SIZE / FLOPPY_SEC) /* 4 floppy sectors/block */

const uint8_t stage2_boot_device = BOOT_DEV_FLOPPY;

/* ── IOCS _B_READ wrapper ───────────────────────────────────────────────── */

static void read_floppy_sector(uint32_t lsec, void *dest) {
  uint32_t cyl = lsec / SECS_PER_CYL;
  uint32_t wcyl = lsec % SECS_PER_CYL;
  uint32_t head = wcyl >> 3;
  uint32_t sec = (wcyl & 7u) + 1u; /* 1-based */

  uint32_t d2 = (SEC_LEN_CODE << 24) | (cyl << 16) | (head << 8) | sec;

  register uint32_t d0 asm("d0") = 0x46u;
  register uint32_t d1 asm("d1") = (FDC_PDA << 8) | FDC_MODE;
  register uint32_t _d2 asm("d2") = d2;
  register uint32_t d3 asm("d3") = FLOPPY_SEC;
  register void *a1 asm("a1") = dest;
  asm volatile("trap #15"
               : "+r"(d0)
               : "r"(d1), "r"(_d2), "r"(d3), "r"(a1)
               : "a0", "memory");
  (void)d0; /* stage1 already verified the boot — ignore FDC status */
}

/* Read one 4 KB UFS block, addressed by its starting fragment number.
 * `frag` must be a multiple of UFS_BLOCK_SIZE/UFS_FRAG_SIZE (block-aligned). */
void read_ufs_block(uint32_t frag, void *dest) {
  /* Fragment N starts at byte UFS_FLOPPY_BASE*1024 + N*512 in the floppy.
   * In 1024-byte sectors: lsec = UFS_FLOPPY_BASE + N/2.  The fragment is
   * block-aligned, so N is a multiple of 8 → N/2 is a multiple of 4. */
  uint32_t lsec = UFS_FLOPPY_BASE + (frag >> 1);
  uint8_t *p = (uint8_t *)dest;
  for (uint32_t i = 0; i < UFS_FLOPPY_SECS; i++) {
    read_floppy_sector(lsec + i, p);
    p += FLOPPY_SEC;
  }
}
