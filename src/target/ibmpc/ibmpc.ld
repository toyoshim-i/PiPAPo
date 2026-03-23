/*
 * ibmpc.ld -- Linker script for IBM PC target (i8086 real mode)
 *
 * P-1 boot sector: BIOS loads sector 0 to 0x7C00.
 * Code + data must fit in 510 bytes (512 - 2 byte signature).
 * For later phases, stage1 will chain-load a larger stage2 + kernel.
 *
 * Memory layout:
 *   0x7C00  .text.entry  (boot entry point)
 *   ...     .text        (code)
 *   ...     .rodata      (read-only data)
 *   ...     .data        (initialized data)
 *   0x7DFE  boot signature (0x55AA)
 *   0x7E00  .bss         (after boot sector)
 *   ...     stack at 0x7C00 (grows downward from entry point)
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
  . = 0x7C00;

  .text : {
    *(.text.entry)
    *(.text .text.*)
  }

  .rodata : {
    *(.rodata .rodata.*)
  }

  .data : {
    *(.data .data.*)
  }

  /* Boot signature at offset 510 from start of sector */
  . = 0x7C00 + 510;
  .bootsig : {
    SHORT(0xAA55)
  }

  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    __bss_end = .;
  }

  /* Stack grows down from 0x7C00 */
  __stack_top = 0x7C00;

  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.eh_frame*)
  }
}
