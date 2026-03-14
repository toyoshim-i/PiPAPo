/*
 * stage2.ld — Linker script for PPAP X68000 Stage 2 UFS loader
 *
 * Stage2 is loaded by stage1 from floppy sectors 1–3 (3072 bytes max)
 * to RAM address 0x003000, and executed there directly.
 *
 * VMA = 0x003000 (execution address)
 * LMA = 0x000000 (file offset 0 for objcopy -O binary)
 *
 * Why 0x003000 is safe:
 *   X68000 system/IOCS work areas occupy 0x000000–0x001FFF (exception
 *   vectors, IOCS work, OPM driver, FDC/DMA/MFP driver work areas).
 *   0x002000–0x005FFF is the "boot area" — used by IPL to load the boot
 *   sector (sector 0 → 0x002000), then free for the boot sector's own use.
 *   IOCS _B_READ DMA writes directly to the caller's a1 buffer (= BUF at
 *   0x000800 in stage2); it does NOT use 0x003000 as an internal buffer.
 *   The FDC interrupt handler works within 0x000C00–0x001FFF.
 *
 * The binary must not exceed 3072 bytes (.text + .rodata).
 * BSS is NOLOAD and lives above the code; stage2_head.S does not need to
 * zero it because stage2.c uses only fixed-address buffers (not .bss arrays).
 */

SECTIONS {
    /* Entry point is in .text.entry (stage2_head.S) */
    .text 0x003000 : AT(0) {
        *(.text.entry)
        *(.text*)
        . = ALIGN(4);
    }

    .rodata : {
        *(.rodata*)
        . = ALIGN(4);
    }

    /* BSS: zero-initialized data above the loaded code (NOLOAD = not in file) */
    .bss (NOLOAD) : {
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
    }
}
