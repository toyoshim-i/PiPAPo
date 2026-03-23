/*
 * ibmpc.ld -- Linker script for IBM PC target (i8086 real mode)
 *
 * Small memory model: all code and data in a single 64 KB segment.
 * The binary is loaded at segment:0000 by the boot loader or QEMU.
 *
 * Memory layout within the segment:
 *   0x0000  .text.entry  (boot entry point, must be first)
 *   ...     .text        (code)
 *   ...     .rodata      (read-only data)
 *   ...     .data        (initialized data)
 *   ...     .bss         (uninitialized data, zeroed by boot.S)
 *   ...     stack        (grows downward from __stack_top)
 *   0xFFFE  __stack_top  (top of 64 KB segment)
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
  . = 0x0000;

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

  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    __bss_end = .;
  }

  /* Stack at top of segment (64 KB boundary) */
  __stack_top = 0xFFFE;

  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.eh_frame*)
  }
}
