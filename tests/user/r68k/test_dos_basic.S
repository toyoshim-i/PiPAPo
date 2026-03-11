| test_dos_basic.S — Human68k DOS basic system call tests (R-format)
|
| Tests non-I/O system calls that require no file system.
| Exit code: 0 = all pass, N = number of failures.
|
| Tested calls:
|   _VERNUM  ($FF30) — OS version
|   _CURDRV  ($FF19) — current drive
|   _GETDATE ($FF2A) — get date
|   _GETTIME ($FF2C) — get time
|   _GETPDB  ($FF51) — get process descriptor block
|   _BREAKCK ($FF33) — break check mode
|   _IOCTRL  ($FF44) — device I/O control (mode 0)
|   _SUPER   ($FF20) — supervisor/user mode toggle

    .text
    .even
    .globl _start

_start:
    moveq   #0, %d7             | d7 = fail counter

    | === _VERNUM ($FF30) ===
    | Expected: 0x36380302 ("68" + version 3.02)
    .word   0xFF30
    cmp.l   #0x36380302, %d0
    beq     1f
    addq.l  #1, %d7
1:

    | === _CURDRV ($FF19) ===
    | Expected: 0 (drive A:)
    .word   0xFF19
    tst.l   %d0
    beq     1f
    addq.l  #1, %d7
1:

    | === _GETDATE ($FF2A) ===
    | Expected: (4<<16)|(46<<9)|(1<<5)|1 = 0x00045C21
    | (Thursday, 2026-01-01)
    .word   0xFF2A
    cmp.l   #0x00045C21, %d0
    beq     1f
    addq.l  #1, %d7
1:

    | === _GETTIME ($FF2C) ===
    | Expected: 0 (00:00:00)
    .word   0xFF2C
    tst.l   %d0
    beq     1f
    addq.l  #1, %d7
1:

    | === _GETPDB ($FF51) ===
    | Expected: non-zero (PMB address)
    .word   0xFF51
    tst.l   %d0
    bne     1f
    addq.l  #1, %d7
1:

    | === _BREAKCK ($FF33) ===
    | mode=0 (get), expected: 1 (break check on)
    clr.w   -(%sp)
    .word   0xFF33
    addq.l  #2, %sp
    cmp.l   #1, %d0
    beq     1f
    addq.l  #1, %d7
1:

    | === _IOCTRL ($FF44) mode 0 on stdout ===
    | Expected: 0x80C1 (character device, console)
    move.w  #1, -(%sp)          | fd = 1 (stdout)
    clr.w   -(%sp)              | mode = 0 (get device info)
    .word   0xFF44
    addq.l  #4, %sp
    cmp.l   #0x80C1, %d0
    beq     1f
    addq.l  #1, %d7
1:

    | === _SUPER ($FF20) enter supervisor ===
    | _SUPER(0): enter supervisor mode, returns previous SSP (non-zero)
    clr.l   -(%sp)
    .word   0xFF20
    addq.l  #4, %sp
    move.l  %d0, %d6            | save returned SSP
    tst.l   %d6
    bne     1f
    addq.l  #1, %d7
1:

    | === _SUPER ($FF20) return to user ===
    | _SUPER(ssp): return to user mode, returns 0
    move.l  %d6, -(%sp)
    .word   0xFF20
    addq.l  #4, %sp
    tst.l   %d0
    beq     1f
    addq.l  #1, %d7
1:

    | === Exit with fail count ===
    move.w  %d7, -(%sp)
    .word   0xFF4C              | _EXIT2
