| =============================================================================
| crt0.S — Minimal C runtime for PPAP m68k user-space programs
| =============================================================================
|
| Entry point: _start
|   1. Read argc/argv from the stack (set up by execve)
|   2. Call main(argc, argv)
|   3. Call _exit(main's return value)
|   4. Dead loop (safety net — _exit should never return)
|
| Stack layout at entry (set by execve):
|   [SP+0] = argc
|   [SP+4] = argv[0], argv[1], ..., argv[argc]=NULL, envp..., auxv...
|
| Linked first via user.ld so _start is at the ELF entry point.
| =============================================================================

    .text
    .even
    .globl _start

_start:
    | Read argc and argv from stack (set up by kernel's execve)
    move.l  (%sp), %d0          | d0 = argc
    lea     4(%sp), %a0         | a0 = argv = &SP[1]

    | Push argv, argc as arguments (m68k C ABI: args on stack)
    move.l  %a0, %sp@-          | push argv
    move.l  %d0, %sp@-          | push argc
    bsr     main
    addq.l  #8, %sp             | clean up 2 args

    | _exit(main's return value in d0)
    move.l  %d0, %d1            | d1 = status (arg 1)
    moveq   #0, %d0             | d0 = SYS_EXIT = 0x0000
    trap    #0

    | Should never reach here
.Lhang:
    bra.s   .Lhang
