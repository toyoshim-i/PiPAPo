@ =============================================================================
@ crt0.S — Minimal C runtime for PPAP user-space programs
@ =============================================================================
@
@ Entry point: _start
@   1. Call main(0, NULL)      — argc=0, argv=NULL (Phase 3 simplicity)
@   2. Call _exit(main's return value)
@   3. Dead loop (safety net — _exit should never return)
@
@ Linked first via user.ld so _start is at the ELF entry point.
@ =============================================================================

.syntax unified
.cpu cortex-m0plus
.thumb

.section .text.crt0, "ax", %progbits
.align 2
.global _start
.thumb_func
_start:
    @ main(argc=0, argv=NULL)
    movs r0, #0
    movs r1, #0
    bl   main

    @ _exit(main's return value in r0)
    movs r7, #1         @ SYS_EXIT = 1
    svc  0

    @ Should never reach here
.Lhang:
    b    .Lhang
