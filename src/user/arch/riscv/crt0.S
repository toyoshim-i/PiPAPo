# =============================================================================
# crt0.S — Minimal C runtime for PPAP user-space programs (RISC-V)
# =============================================================================
#
# Entry point: _start
#   1. Read argc/argv from the stack (set up by do_execve)
#   2. Call main(argc, argv)
#   3. Call _exit(main's return value)
#   4. Dead loop (safety net — _exit should never return)
#
# Stack layout at entry (set by do_execve):
#   [SP+0] = argc
#   [SP+4] = argv[0], argv[1], ..., argv[argc]=NULL, envp..., auxv...
#
# Linked first via user.ld so _start is at the ELF entry point.
# =============================================================================

.section .text.crt0, "ax"
.align 2
.global _start
_start:
    # Read argc and argv from stack (set up by kernel's do_execve)
    lw   a0, 0(sp)         # a0 = argc
    addi a1, sp, 4          # a1 = argv = &SP[1]
    call main

    # _exit(main's return value in a0)
    li   a7, 0              # SYS_EXIT = 0x0000
    ecall

    # Should never reach here
.Lhang:
    j    .Lhang
