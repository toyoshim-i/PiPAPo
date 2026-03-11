| test_dos_rename.S — Human68k _RENAME DOS call test (R-format)
|
| Tests: _CREATE, _RENAME, _OPEN (new name), _OPEN (old name fails), _DELETE
| Requires /tmp (tmpfs) to be mounted.
| Exit code: 0 = all pass, N = number of failures.

    .text
    .even
    .globl _start

_start:
    moveq   #0, %d7             | d7 = fail counter

    | === _CREATE("/tmp/ren_old.dat", 0x20) ===
    move.w  #0x20, -(%sp)       | attr = archive
    pea     .Lold(%pc)
    .word   0xFF3C              | _CREATE
    addq.l  #6, %sp
    tst.l   %d0
    bpl     1f
    addq.l  #1, %d7
    bra     .Lexit
1:
    | Close the created file
    move.w  %d0, -(%sp)
    .word   0xFF3E              | _CLOSE
    addq.l  #2, %sp

    | === _RENAME("/tmp/ren_old.dat", "/tmp/ren_new.dat") ===
    | Stack: long old_path, long new_path
    pea     .Lnew(%pc)
    pea     .Lold(%pc)
    .word   0xFF56              | _RENAME
    addq.l  #8, %sp
    tst.l   %d0
    beq     1f
    addq.l  #1, %d7
1:

    | === _OPEN("/tmp/ren_new.dat", 0) — should succeed ===
    clr.w   -(%sp)
    pea     .Lnew(%pc)
    .word   0xFF3D              | _OPEN
    addq.l  #6, %sp
    tst.l   %d0
    bpl     1f
    addq.l  #1, %d7
    bra     .Lskip_open
1:
    move.w  %d0, -(%sp)
    .word   0xFF3E              | _CLOSE
    addq.l  #2, %sp
.Lskip_open:

    | === _OPEN("/tmp/ren_old.dat", 0) — should fail (renamed away) ===
    clr.w   -(%sp)
    pea     .Lold(%pc)
    .word   0xFF3D              | _OPEN
    addq.l  #6, %sp
    tst.l   %d0
    bmi     1f                  | negative = error (expected)
    | Unexpected success — close and fail
    move.w  %d0, -(%sp)
    .word   0xFF3E
    addq.l  #2, %sp
    addq.l  #1, %d7
1:

    | === _DELETE("/tmp/ren_new.dat") — cleanup ===
    pea     .Lnew(%pc)
    .word   0xFF41              | _DELETE
    addq.l  #4, %sp

.Lexit:
    move.w  %d7, -(%sp)
    .word   0xFF4C              | _EXIT2

    .even
.Lold:
    .asciz  "/tmp/ren_old.dat"
    .even
.Lnew:
    .asciz  "/tmp/ren_new.dat"
