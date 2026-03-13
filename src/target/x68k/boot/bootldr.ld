/*
 * bootldr.ld — Linker script for PPAP X68000 boot sector
 *
 * Code runs at VMA 0x002000 (loaded there by IPL ROM).
 * Binary starts at LMA 0x000000 so objcopy -O binary outputs
 * code bytes from offset 0 (no leading zero padding).
 */

SECTIONS {
    .text 0x002000 : AT(0x000000) {
        *(.text)
        *(.text*)
    }
}
