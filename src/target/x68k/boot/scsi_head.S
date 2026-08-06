| =============================================================================
| scsi_head.S — PPAP X68000 SCSI IPL loader entry (disk record 1 → 0x002000)
| =============================================================================
|
| The ROM SCSIINROM boot handle reads disk record 1 (1024 bytes) to 0x002000,
| verifies the first byte is $60 (BRA), and JSRs 0x002000 with:
|   D4.b = boot SCSI ID   A1 = 0x002000 (load address / entry)
|
| Unlike the floppy (stage1 sector 0 → stage2 sectors 1-3), the whole SCSI
| loader fits in the single 1024-byte record the ROM loads, so this entry
| calls the shared stage2 core directly — no second on-disk stage.  It:
|   1. Saves the IPL IOCS (TRAP #15) vector for stage2_final().
|   2. Sets a fresh supervisor stack.
|   3. Calls stage2_main(), which loads /boot/kernel from the SCSI UFS
|      (via stage2_scsi.c) and boots it.
|
| Entered via JSR, so a return would fall back to the ROM (boot fails); this
| entry never returns.
| =============================================================================

    .text
    .section .text.entry
    .align 2

    .globl _start
_start:
    bra.s   boot_start              | first byte $60 — SCSIINROM requires it
    .ascii  "PPAP"                  | signature/pad (skipped by bra.s)

boot_start:
    | Save the IPL IOCS vector: TRAP #15 = vector 47 → 47×4 = 0x0000BC.
    | stage2_final() restores it into the kernel vector table.
    | (0x002FF0 = BOOT_HANDOFF_IOCS_SAVE in boot_handoff.h.)
    move.l  0x000000BC,%d0
    move.l  %d0,0x002FF0

    | Fresh stack: grows downward from 0x005F00, well above this loader
    | (0x002000-0x0023FF) and clear of the IOCS/OPM/FDC work areas below
    | 0x002000; the kernel load area (0x006000+) is not populated yet.
    | Matches stage2_head.S so the stack choice is uniform across boot paths.
    lea.l   0x005F00,%sp

    | Call the shared stage2 core (loads kernel via the SCSI backend, then
    | copies vectors and jumps to the kernel — does not return).
    jsr     stage2_main

    | stage2_main() does not return; halt defensively if it ever does.
.halt:
    stop    #0x2700
    bra.s   .halt
