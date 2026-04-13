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
 *   0xB400-0xE37F  Reserved for VFS module DS=0 data + bss
 *   0xE380-0xFFFF  Per-process kernel stacks (slot 0: 128B, slots 1-7: 1KB)
 *   0x10000+       __page_pool_start (free pages)
 *
 * Stage2 loads the kernel binary to DS:0x0600 (for data access via
 * SS=0) and also to a far segment (for code execution via separate
 * CS).  All symbols are linked at DS=0 addresses; the far copy has
 * the same offsets so near calls resolve correctly in both copies.
 *
 * VFS code lives in a separate segment (after core far copy).
 * VFS data lives at 0xB400 in this shared DS=0 space.
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
    /* ecpu_names is zero-init (no eCPU on pcxt) and would be GC'd
       since core never references it, but VFS procfs needs it. */
    KEEP(*subsys.c.obj(.bss))
    __bss_end = .;
  }

  /* __stack_top right after BSS: used by page.c diagnostics.
   * Must stay contiguous with BSS to avoid R_386_16 overflow.
   * The actual boot SP is set to 0xF400 (kernel_stack[0] top)
   * explicitly in boot.S. */
  . = ALIGN(2);
  __stack_top = .;

  /* Reserved for VFS DS=0 data (must match pcxt_vfs.ld). */
  . = 0xB400;
  __vfs_data_reserved = .;

  /* Per-process kernel stacks: slot 0 = 128B (idle loop only),
   * slots 1-7 = 1 KB each.  Pushed to end of DS=0 so the saved
   * 896 bytes from slot 0's reduction extends VFS data headroom.
   * Boot uses SP=0xFFFC (slot 7 top); sched_start replants canaries
   * after boot stack usage is done. */
  . = 0xE380;
  __kstack_base = .;
  . += 7296;  /* 128 + 7 × 1024 = ends at 0x10000 */

  /* Page pool: free pages start outside the first segment */
  . = ALIGN(4096);
  __page_pool_start = .;

  /* Sanity checks */
  ASSERT((__bss_end <= 0xB400),
         "ERROR: Kernel BSS overflows into VFS reserved area (0xB400)")
  ASSERT((__page_pool_start <= 0x9FC00),
         "ERROR: Page pool starts beyond conventional memory (640 KB)")

  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.eh_frame*)
  }
}
