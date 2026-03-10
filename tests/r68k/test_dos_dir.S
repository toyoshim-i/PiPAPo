| test_dos_dir.S — Human68k DOS directory operation tests (R-format)
|
| Tests: _MKDIR, _CHDIR, _CURDIR, _RMDIR
| Requires /tmp (tmpfs) to be mounted.
| Exit code: 0 = all pass, N = number of failures.

    .text
    .even
    .globl _start

_start:
    moveq   #0, %d7             | d7 = fail counter

    | === _MKDIR("/tmp/h68kdir") ===
    | Stack: long path
    pea     .Ldir(%pc)
    .word   0xFF39              | _MKDIR
    addq.l  #4, %sp
    tst.l   %d0
    beq     1f
    addq.l  #1, %d7
    bra     .Lexit              | cannot continue without dir
1:

    | === _CHDIR("/tmp/h68kdir") ===
    pea     .Ldir(%pc)
    .word   0xFF3B              | _CHDIR
    addq.l  #4, %sp
    tst.l   %d0
    beq     1f
    addq.l  #1, %d7
1:

    | === _CURDIR(0, buf) ===
    | Stack: word drive, long buf
    | Allocate 68 bytes on stack for the path buffer
    sub.l   #68, %sp
    move.l  %sp, %a3            | a3 = buffer

    move.l  %a3, -(%sp)         | buf
    clr.w   -(%sp)              | drive = 0 (default)
    .word   0xFF47              | _CURDIR
    addq.l  #6, %sp
    tst.l   %d0
    beq     1f
    addq.l  #1, %d7
    bra     .Lcurdir_done
1:

    | Verify buffer starts with "tmp" (curdir strips leading /)
    | Expected: "tmp\h68kdir" or similar (backslash from dos_curdir)
    cmp.b   #'t', (%a3)
    bne     2f
    cmp.b   #'m', 1(%a3)
    bne     2f
    cmp.b   #'p', 2(%a3)
    bne     2f
    bra     1f
2:
    addq.l  #1, %d7
1:
.Lcurdir_done:
    lea     68(%sp), %sp        | free buffer

    | === _CHDIR("/") — return to root ===
    pea     .Lroot(%pc)
    .word   0xFF3B              | _CHDIR
    addq.l  #4, %sp

    | === _RMDIR("/tmp/h68kdir") ===
    pea     .Ldir(%pc)
    .word   0xFF3A              | _RMDIR
    addq.l  #4, %sp
    tst.l   %d0
    beq     1f
    addq.l  #1, %d7
1:

    | === _RMDIR on non-existent dir — should fail ===
    pea     .Ldir(%pc)
    .word   0xFF3A              | _RMDIR
    addq.l  #4, %sp
    tst.l   %d0
    bmi     1f                  | negative = error (expected)
    addq.l  #1, %d7
1:

.Lexit:
    move.w  %d7, -(%sp)
    .word   0xFF4C              | _EXIT2

    .even
.Ldir:
    .asciz  "/tmp/h68kdir"
    .even
.Lroot:
    .asciz  "/"
