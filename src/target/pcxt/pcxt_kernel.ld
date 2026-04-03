/*
 * pcxt_kernel.ld — Core module linker script (small model, DS=0)
 *
 * Memory layout (shared DS=0 linear address space):
 *
 *   0x0000-0x05FF  IVT + BIOS data + mod_info
 *   0x0600+        .text (core code, also copied to far CS by stage2)
 *                  .rodata (constants)
 *                  .data (core initialized globals)
 *                  .bss (core zero-initialized globals)
 *   0xA000-0xEFFF  Reserved for VFS module DS=0 data + bss
 *   0xF000-0xFFFF  Per-process kernel stacks (4 × 1 KB)
 *   0x10000+       __page_pool_start (free pages)
 *
 * Stage2 loads the kernel binary to DS:0x0600 (for data access via
 * SS=0) and also to a far segment (for code execution via separate
 * CS).  All symbols are linked at DS=0 addresses; the far copy has
 * the same offsets so near calls resolve correctly in both copies.
 *
 * VFS code lives in a separate segment (after core far copy).
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

  /* __stack_top right after BSS: used by page.c diagnostics.
   * Must stay contiguous with BSS to avoid R_386_16 overflow.
   * The actual boot SP is set to 0xF400 (kernel_stack[0] top)
   * explicitly in boot.S. */
  . = ALIGN(2);
  __stack_top = .;

  /* Reserved for VFS DS=0 data (must match pcxt_vfs.ld). */
  . = 0xA000;
  __vfs_data_reserved = .;

  /* Per-process kernel stacks: 4 × 2 KB at top of DS=0 segment.
   * 1 KB is marginal — sys_execve + VFS far calls + floppy reads
   * approach ~800 bytes; debug klogf calls push it over.
   * PROC_MAX=4, one stack per process slot.  kernel_stack[0]
   * doubles as the boot stack (SP=0xE800 in boot.S). */
  . = 0xE000;
  __kstack_base = .;
  . += 8192;  /* 4 × 2048 */

  /* Page pool: free pages start outside the first segment */
  . = ALIGN(4096);
  __page_pool_start = .;

  /* Sanity checks */
  ASSERT((__bss_end <= 0xA000),
         "ERROR: Kernel BSS overflows into VFS reserved area (0xA000)")
  ASSERT((__page_pool_start <= 0x9FC00),
         "ERROR: Page pool starts beyond conventional memory (640 KB)")

  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.eh_frame*)
  }
}
