/*
 * scsi_loader.ld — Linker script for the PPAP X68000 SCSI IPL loader.
 *
 * The ROM SCSIINROM reads disk record 1 (1024 bytes) to RAM 0x002000 and
 * executes it there, so the loader (scsi_head.S + stage2_core.c +
 * stage2_scsi.c) links at VMA 0x002000.  The binary must not exceed 1024
 * bytes (.text + .rodata); mkx68kimg.sh enforces this.
 *
 * VMA = 0x002000 (execution address); LMA = 0x000000 (objcopy -O binary).
 * BSS is NOLOAD above the code; the core uses only fixed-address buffers.
 */

SECTIONS {
    /* Entry point is in .text.entry (scsi_head.S) — must start at 0x002000. */
    .text 0x002000 : AT(0) {
        *(.text.entry)
        *(.text*)
        . = ALIGN(4);
    }

    .rodata : {
        *(.rodata*)
        . = ALIGN(4);
    }

    .bss (NOLOAD) : {
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
    }
}
