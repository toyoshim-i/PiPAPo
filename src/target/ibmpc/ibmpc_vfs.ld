/*
 * ibmpc_vfs.ld — VFS module linker script (separate code segment)
 *
 * Code (.text) starts at offset 0 within the VFS segment.
 * Data (.data/.bss) is placed at a fixed address in the shared
 * DS=0 range so both core and VFS can access it.
 *
 * The .text.stubs section is placed FIRST so target-side stubs
 * have known, stable offsets (0x0000, 0x0004, 0x0008, ...).
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)

SECTIONS
{
  /* Code at offset 0 — stubs first for stable offsets */
  . = 0x0000;
  .text : {
    *(.text.stubs)
    *(.text .text.*)
  }
  .rodata : { *(.rodata .rodata.*) }

  /* VFS data in shared DS=0 range.
   * Must not overlap with core data (core uses 0x0600-0x7FFF). */
  . = 0xA000;
  .data : { *(.data .data.*) }
  .bss (NOLOAD) : {
    *(.bss .bss.*)
    *(COMMON)
  }

  /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame*) }
}
