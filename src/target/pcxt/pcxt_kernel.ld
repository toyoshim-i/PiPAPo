/*
 * pcxt_kernel.ld — Core module linker script (small model, DS=0)
 *
 * Memory layout (shared DS=0 linear address space):
 *
 *   0x0000-0x05FF  IVT + BIOS data
 *   0x0600+        .text (core code)
 *                  .rodata (constants)
 *                  .data (core initialized globals)
 *                  .bss (core zero-initialized globals)
 *                  2 KB stack
 *   0xA000-0xFFFF  Reserved for VFS module DS=0 data + bss
 *   0x10000+       __page_pool_start (free pages)
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

  __core_cs_end = .;

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
  __stack_bottom = .;
  . += 2048;
  __stack_top = .;

  /* Reserved for VFS DS=0 data (must match pcxt_vfs.ld).  The entire
   * remainder of the first segment stays out of the page allocator. */
  . = 0xA000;
  __vfs_data_reserved = .;
  . = 0x10000;

  /* Page pool: free pages start outside the first segment */
  . = ALIGN(4096);
  __page_pool_start = .;

  /* Sanity checks */
  ASSERT((__stack_top <= 0xA000),
         "ERROR: Kernel stack overflows into VFS reserved area (0xA000)")
  ASSERT((__page_pool_start <= 0x9FC00),
         "ERROR: Page pool starts beyond conventional memory (640 KB)")

  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.eh_frame*)
  }
}
