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
|
| Stack placement:
|   X68000 system work area layout (must stay clear):
|     $000000-$0003FF  CPU exception vectors
|     $000400-$0007FF  IOCS work area (cursor, screen config ...)
|     $000800-$000FFF  OPM (YM2151) driver work area
|     $001000-$001FFF  FDC / SCSI driver work area
|
|   stage2 is loaded at $003000-$003BFF; free space above it extends to
|   $005FFF.  SSP = $005F00 keeps the stack well above all driver work
|   areas ($001FFF) and well below the kernel load address ($006000).
|
|   Human68K uses $6800 as initial SSP; we cannot match that because
|   load_file() writes the kernel binary to $006000 and would overwrite
|   any stack frames living above $006000.
| =============================================================================

    .text
    .section .text.entry
    .align 2

    .globl _start
_start:
    | Fresh stack: grows downward from 0x005F00.
    | Free area $003C00-$005FFF is clear of IOCS work areas, stage2 code,
    | and the kernel load target ($006000).
    lea.l   0x005F00,%sp

    | Call C stage2 main function
    jsr     stage2_main

    | Should never return; halt if it does
.halt:
    stop    #0x2700
    bra.s   .halt
