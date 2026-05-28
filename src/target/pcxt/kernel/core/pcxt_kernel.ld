/*
 * pcxt_kernel.ld — Core module linker script (small model)
 *
 * Two independent VMA regions, one per segment register:
 *
 *   CS region (own 64 KB segment, runtime CS = 0x1000):
 *     0x0000+  .text   (core code only)
 *
 *   DS=0 region (shared 64 KB segment, runtime SS = DS = 0):
 *     0x0000-0x05FF  IVT + BIOS data + mod_info
 *     0x0600+        .rodata + .data + .bss  (core read-only and r/w data)
 *     0xB600-0xE37F  Reserved for VFS module DS=0 data + bss
 *     0xE380-0xFFFF  Per-process kernel stacks
 *     0x10000+       __page_pool_start (free pages)
 *
 * `.rodata` stays in DS=0 because ia16-elf-gcc's small model addresses
 * read-only data via `%ss:` (see docs/targets/ia16.md §5).  Only `.text`
 * (executed via CS) can be moved out of DS=0.
 *
 * Stage2 loads .text to CS:0x0000 (the only copy — no DS=0 dup) and
 * .rodata+.data to DS:0x0600.  Each section's symbols stay in their
 * own 16-bit VMA range so R_386_16 relocations always fit:
 *   - function-pointer-in-data references a text VMA ≤ 0xFFFF
 *   - data-pointer-in-code references a data VMA ≤ 0xFFFF
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
  /* ── Code region (CS = 0x1000 at runtime) ────────────────────────── */
  . = 0x0000;

  .text : {
    *(.text.entry)
    *(.text .text.*)
  }

  __core_cs_end = .;

  /* ── Data region (DS = SS = 0 at runtime) ───────────────────────── */
  . = 0x0600;

  __core_data_lma = ALIGN(LOADADDR(.text) + SIZEOF(.text), 16);

  .rodata : AT(__core_data_lma) {
    *(.rodata .rodata.*)
  }

  .data : {
    *(.data .data.*)
    /* Name arrays are exported to the separately linked VFS module.
       Keep initialized names even when core has no local reference. */
    KEEP(*subsys.c.obj(.data))
    KEEP(*subsys.c.obj(.data.*))
  }

  __romfs_start = .;
  __romfs_end = .;

  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    /* Name arrays are exported to the separately linked VFS module.
       Keep zero-init names even when core has no local reference. */
    KEEP(*subsys.c.obj(.bss))
    KEEP(*subsys.c.obj(.bss.*))
    __bss_end = .;
  }

  /* __stack_top right after BSS: used by page.c diagnostics.
   * The actual boot SP is set to 0xF400 (kernel_stack[0] top)
   * explicitly in boot.S. */
  . = ALIGN(2);
  __stack_top = .;

  /* Reserved for VFS DS=0 data (must match pcxt_vfs.ld). */
  . = 0xB600;
  __vfs_data_reserved = .;

  /* Per-process kernel stacks: slot 0 = 128B (idle loop only),
   * slots 1-7 = 1 KB each.  Boot uses SP=0xFFFC (slot 7 top);
   * sched_start replants canaries after boot stack usage is done.
   * __kstack_region_base is the symbol read by the shared
   * proc_kstack_init_slot() impl. */
  . = 0xE380;
  __kstack_region_base = .;
  . += 7296;  /* 128 + 7 × 1024 = ends at 0x10000 */

  /* Page pool: free pages start outside the first segment */
  . = ALIGN(4096);
  __page_pool_start = .;

  /* Sanity checks */
  ASSERT((__core_cs_end <= 0x10000),
         "ERROR: Core .text+.rodata exceeds 64 KB CS segment")
  ASSERT((__bss_end <= 0xB600),
         "ERROR: Core .data+.bss overflows into VFS reservation")
  ASSERT((__page_pool_start <= 0x9FC00),
         "ERROR: Page pool starts beyond conventional memory (640 KB)")

  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.eh_frame*)
  }
}
