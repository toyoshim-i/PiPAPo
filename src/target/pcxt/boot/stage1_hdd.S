/*
 * stage1_hdd.S — IBM PC MBR boot sector (sector 0, 512 bytes)
 *
 * BIOS loads this to 0x0000:0x7C00 and jumps here.
 * Loads 8 sectors (stage2, 4 KB) from HDD to 0x0000:0xC000,
 * then jumps to stage2 entry at 0x0000:0xC000.
 *
 * The MBR partition table (offset 446-509) is filled by mkpcimg.sh
 * at image build time, not embedded here.
 */

  .code16
  .section .text, "ax"
  .global _start

_start:
  jmp   .Lstart
  nop

  /* ── Boot code (offset 3) ─────────────────────────────────────────── */

  .include "stage1_common.inc"

  /* ── MBR partition table (offset 446) ─────────────────────────────── */
  /* Left as zeros — mkpcimg.sh patches the partition entries and
   * boot signature into the final image. */
  .org  446
  .space 64, 0        /* 4 × 16-byte partition entries */

  /* Boot signature */
  .word 0xAA55
