/*
 * pcxt.ld -- Linker script for PC/XT target (i8086 real mode)
 *
 * Two-part layout within a single binary:
 *   Sector 0 (0x7C00-0x7DFF): boot.S — loads remaining sectors, jumps to kmain
 *   Sector 1+ (0x7E00-...):   kernel code, data, BSS, stack
 *
 * BIOS loads sector 0 to 0x0000:0x7C00.  boot.S uses INT 13h to read
 * the remaining sectors into 0x7E00+, then calls kmain().
 *
 * All segments = 0 (flat real-mode addressing with 16-bit near pointers).
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
  /* ---- Boot sector (sector 0, 512 bytes at 0x7C00) ---- */
  . = 0x7C00;

  .boot : {
    *(.text.entry)
  }

  /* Boot signature at byte 510 */
  . = 0x7C00 + 510;
  .bootsig : {
    SHORT(0xAA55)
  }

  /* ---- Kernel (sector 1+, loaded at 0x7E00 by boot.S) ---- */
  . = 0x7E00;
  __kernel_start = .;

  .text : {
    *(.text .text.*)
  }

  .rodata : {
    *(.rodata .rodata.*)
  }

  .data : {
    *(.data .data.*)
  }

  __kernel_end = .;

  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    __bss_end = .;
  }

  /* Stack: 2 KB after BSS */
  . = ALIGN(2);
  __stack_bottom = .;
  . += 2048;
  __stack_top = .;

  /* Number of extra sectors to load (kernel size / 512, rounded up) */
  __kernel_sectors = (__kernel_end - 0x7E00 + 511) / 512;

  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.eh_frame*)
  }
}
