/*
 * ibmpc_kernel.ld — Kernel linker script (medium model)
 *
 * Memory layout (all in DS=0, linear address space):
 *
 *   0x0000-0x05FF  IVT + BIOS data
 *   0x0600-....    .text (near code: boot, switch, trap ISRs)
 *                  .fartext (far code: all C functions via -mcmodel=medium)
 *                  .rodata (constants)
 *                  .data (initialized globals)
 *                  .bss (zero-initialized globals)
 *                  2 KB stack
 *   ALIGN(4096)    __page_pool_start (free pages for kernel use)
 *
 * The medium model puts most C code in .fartext sections (accessed via
 * lcall/lret). Data stays near (16-bit DS-relative pointers).
 * Near .text is reserved for assembly that must be directly callable
 * from IVT entries (interrupt handlers).
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
  . = 0x0600;

  /* Near code: boot.S, switch.S, trap.S (must be near for
   * direct jmp/call from IVT entries and context switch) */
  .text : {
    *(.text.entry)
    *(.text .text.*)
  }

  /* Far code: all C functions compiled with -mcmodel=medium.
   * Accessed via far calls. DS=0 is still used for data access.
   * Placed right after near text so it's contiguous with code. */
  .fartext : {
    *(.fartext .fartext.*)
  }

  /* Read-only data (near, DS=0) */
  .rodata : {
    *(.rodata .rodata.*)
  }

  /* Initialized data (near, DS=0) */
  .data : {
    *(.data .data.*)
  }

  __romfs_start = .;
  __romfs_end = .;

  /* BSS (near, DS=0) */
  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    __bss_end = .;
  }

  /* Stack (near, 2 KB) */
  . = ALIGN(2);
  . += 2048;
  __stack_top = .;

  /* Page pool: free pages start here */
  . = ALIGN(4096);
  __page_pool_start = .;

  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.eh_frame*)
  }
}
