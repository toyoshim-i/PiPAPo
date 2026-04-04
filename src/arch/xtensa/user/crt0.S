# =============================================================================
# crt0.S — Minimal C runtime for PPAP user-space programs (Xtensa)
# =============================================================================
#
# Entry point: _start
#   1. Read argc/argv from the stack (set up by execve)
#   2. Call main(argc, argv)
#   3. Call _exit(main's return value)
#   4. Dead loop (safety net — _exit should never return)
#
# Stack layout at entry (set by execve):
#   [SP+0] = argc
#   [SP+4] = argv[0], argv[1], ..., argv[argc]=NULL, envp..., auxv...
#
# Uses call0 ABI for user binaries (simpler syscall interface).
# =============================================================================

    .section .text.crt0, "ax"
    .align 4
    .global _start
_start:
    l32i    a2, a1, 0           # a2 = argc
    addi    a3, a1, 4           # a3 = argv = &SP[1]
    call0   main                # call0 ABI: main(argc, argv)

    # _exit(main's return value in a2)
    movi    a7, 0               # SYS_EXIT = 0x0000
    ill                         # PPAP syscall trap (EXCCAUSE=0)

    # Should never reach here
.Lhang:
    j       .Lhang
