/*
 * ibmpc_kernel.ld — Core module linker script (small model, DS=0)
 *
 * Memory layout (shared DS=0 linear address space):
 *
 *   0x0000-0x05FF  IVT + BIOS data
 *   0x0600+        .text (core code)
 *                  .rodata (constants)
 *                  .data (core initialized globals)
 *                  .bss (core zero-initialized globals)
 *                  2 KB stack
 *   0xA000-0xBFFF  Reserved for VFS module .data + .bss
 *   ALIGN(4096)    __page_pool_start (free pages)
 *
 * VFS code lives in a separate segment (CS=0x1000).
 * VFS data lives at 0xA000 in this shared DS=0 space.
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

  __romfs_start = .;
  __romfs_end = .;

  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    __bss_end = .;
  }

  /* Stack (2 KB) */
  . = ALIGN(2);
  . += 2048;
  __stack_top = .;

  /* Reserved for VFS module data (must match ibmpc_vfs.ld) */
  . = 0xA000;
  __vfs_data_reserved = .;
  . += 8192;   /* 8 KB for VFS .data + .bss */

  /* Page pool: free pages start here */
  . = ALIGN(4096);
  __page_pool_start = .;

  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.eh_frame*)
  }
}
