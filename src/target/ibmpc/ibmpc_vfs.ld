/*
 * ibmpc_vfs.ld — VFS module linker script (small model, own segment)
 *
 * The VFS module is loaded at a paragraph-aligned address by stage2.
 * CS and DS are both set to the module's segment base on entry.
 * All code + data + BSS must fit in 64 KB.
 *
 * The module starts at offset 0 within its segment.  The segment
 * base is assigned at runtime by the segment manager.
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)

SECTIONS
{
  . = 0x0000;

  .text : {
    *(.text.header)   /* Module header (entry table) must be at offset 0 */
    *(.text .text.*)
  }

  .rodata : {
    *(.rodata .rodata.*)
  }

  .data : {
    *(.data .data.*)
  }

  .bss (NOLOAD) : {
    __vfs_bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    __vfs_bss_end = .;
  }

  _end = .;

  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.eh_frame*)
  }
}
