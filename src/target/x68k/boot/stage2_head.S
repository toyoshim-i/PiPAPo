| =============================================================================
| stage2_head.S — Stage 2 Entry Point (runs at 0x003000)
| =============================================================================
|
| This stub is the first code executed by stage2.  It must be linked as the
| very first section (.text.entry) so it starts at VMA 0x003000.
|
| Responsibilities:
|   1. Reset the supervisor stack pointer (stage1 may have partially used it)
|   2. Call stage2_main() (C function in stage2.c)
|   3. Halt on return (stage2_main() does not return normally —
|      it jumps to the kernel via stage2_final())
| =============================================================================

    .text
    .section .text.entry
    .align 2

    .globl _start
_start:
    | Fresh stack: grows downward from 0x002000.
    | stage1 used this same stack, but since we are starting fresh here
    | there are no saved frames to preserve.
    lea.l   0x002000,%sp

    | Call C stage2 main function
    jsr     stage2_main

    | Should never return; halt if it does
.halt:
    stop    #0x2700
    bra.s   .halt
