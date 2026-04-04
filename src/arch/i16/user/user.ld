/*
 * user.ld — Linker script for PPAP i16 user-space ELF binaries
 *
 * Static ET_EXEC binaries linked at address 0 for elf16_loader.
 * Code, rodata, data, and bss live in one contiguous PT_LOAD region.
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

PHDRS
{
  text PT_LOAD FLAGS(7);  /* PF_R | PF_W | PF_X */
}

SECTIONS
{
  . = 0;

  .text : {
    *(.text.crt0)
    *(.text*)
  } :text

  .rodata : {
    *(.rodata*)
  } :text

  . = ALIGN(2);

  .data : {
    *(.data*)
  } :text

  .bss : {
    __bss_start = .;
    *(.bss*)
    *(COMMON)
    __bss_end = .;
  } :text

  _end = .;

  /DISCARD/ : {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
  }
}
