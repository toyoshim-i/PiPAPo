@ =============================================================================
@ crt0.S — Minimal C runtime for PPAP user-space programs
@ =============================================================================
@
@ Entry point: _start
@   1. Read argc/argv from the stack (set up by do_execve)
@   2. Call main(argc, argv)
@   3. Call _exit(main's return value)
@   4. Dead loop (safety net — _exit should never return)
@
@ Stack layout at entry (set by do_execve):
@   [SP+0] = argc
@   [SP+4] = argv[0], argv[1], ..., argv[argc]=NULL, envp..., auxv...
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
    @ Read argc and argv from stack (set up by kernel's do_execve)
    ldr  r0, [sp]          @ r0 = argc
    mov  r1, sp
    adds r1, #4            @ r1 = argv = &SP[1]
    bl   main

    @ _exit(main's return value in r0)
    movs r7, #1         @ SYS_EXIT = 1
    svc  0

    @ Should never reach here
.Lhang:
    b    .Lhang
