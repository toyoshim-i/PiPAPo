/*
 * ibmpc_kernel.ld — Kernel linker script
 *
 * Stage2 loads this flat binary to 0x3000 and jumps here.
 * All segments = 0 (flat real-mode addressing).
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
  . = 0x0600;

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

  /* No romfs embedded — loaded from floppy UFS */
  __romfs_start = .;
  __romfs_end = .;

  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    __bss_end = .;
  }

  /* Stack: 2 KB after BSS */
  . = ALIGN(2);
  . += 2048;
  __stack_top = .;

  /* Page pool starts after stack (aligned to PAGE_SIZE=4096) */
  . = ALIGN(4096);
  __page_pool_start = .;

  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.eh_frame*)
  }
}
