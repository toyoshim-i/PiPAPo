/*
 * ibmpc_vfs.ld — VFS module linker script (shared DS=0)
 *
 * Code (.text, .rodata) starts at offset 0 in the VFS code segment.
 * Data (.data, .bss) is placed at 0xA000 in the shared DS=0 space.
 *
 * The compiler generates DS-relative data access (e.g. mov ax,[0xA0xx]).
 * Since DS=0 at runtime, this correctly accesses linear 0xA0xx.
 *
 * The VFS code binary is extracted with:
 *   objcopy -j .text -j .text.header -j .rodata -O binary → code
 * The VFS data binary is extracted with:
 *   objcopy -j .data -O binary → data (copied to DS:0xA000 at boot)
 * BSS is zeroed by the core at boot.
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)

SECTIONS
{
  /* Code segment: offset 0 within VFS CS */
  . = 0x0000;

  .text : {
    *(.text.header)   /* Module header (entry table) at offset 0 */
    *(.text .text.*)
  }

  _code_end = .;

  /* Data follows code contiguously within the same segment.
   * Small-model requires CS=DS.  Stage2 loads VFS at a fixed
   * offset in DS=0 (e.g. 0x8000).  Core and VFS share CS=DS=0. */
  . = ALIGN(16);

  __vfs_data_start = .;
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

  __vfs_data_end = .;

  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.eh_frame*)
  }
}
