/*
 * ibmpc_kernel.ld — Kernel linker script (medium model)
 *
 * Layout: near code (.text) + data + BSS + stack in low addresses
 * (< 0xFFFF for near pointers), then far code (.fartext) above.
 *
 * The medium model (-mcmodel=medium) puts most code in .fartext
 * sections (accessed via lcall/lret). Data stays near (DS=0).
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
  . = 0x0600;

  /* Near code: boot.S, switch.S, trap.S (small, must be near for
   * direct jmp/call from IVT entries) */
  .text : {
    *(.text.entry)
    *(.text .text.*)
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

  /* Page pool: one 4 KB page in near range */
  . = ALIGN(4096);
  __page_pool_start = .;

  /* Far code: all C functions compiled with -mcmodel=medium.
   * These are accessed via lcall (far pointers in mod_* structs)
   * and can be above 0xFFFF. DS=0 is still used for data access. */
  .fartext : {
    *(.fartext .fartext.*)
  }

  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.eh_frame*)
  }
}
