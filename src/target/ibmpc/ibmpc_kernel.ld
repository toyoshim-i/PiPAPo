/*
 * ibmpc_kernel.ld — Kernel linker script (loaded by stage2 at 0x10000)
 *
 * Stage2 loads this flat binary to linear 0x10000 and far-jumps
 * to 0x0000:0x10000 (segment 0, offset 0x10000 won't work — offset
 * must be <64KB).  Instead, load at linear 0x2000 (above stage2 + scratch)
 * and near-jump within segment 0.
 *
 * All segments = 0 (flat real-mode addressing).
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

SECTIONS
{
  . = 0x3000;

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

  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    __bss_end = .;
  }

  /* Stack: 2 KB after BSS */
  . = ALIGN(2);
  . += 2048;
  __stack_top = .;

  /DISCARD/ : {
    *(.comment)
    *(.note.*)
    *(.eh_frame*)
  }
}
