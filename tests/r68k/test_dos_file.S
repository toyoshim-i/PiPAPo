| test_dos_file.S — Human68k DOS file I/O tests (R-format)
|
| Tests: _CREATE, _WRITE, _SEEK, _READ, _CLOSE, _OPEN, _DELETE
| Requires /tmp (tmpfs) to be mounted.
| Exit code: 0 = all pass, N = number of failures.

    .text
    .even
    .globl _start

_start:
    moveq   #0, %d7             | d7 = fail counter

    | === _CREATE("/tmp/h68k_test.dat", 0x20) ===
    | Stack: long path, word attr
    move.w  #0x20, -(%sp)       | attr = archive
    pea     .Lpath(%pc)
    .word   0xFF3C              | _CREATE
    addq.l  #6, %sp
    tst.l   %d0
    bpl     1f                  | d0 >= 0 → file handle
    addq.l  #1, %d7
    bra     .Lexit              | cannot continue without handle
1:
    move.l  %d0, %d6            | d6 = file handle

    | === _WRITE(fd, "Hello!", 6) ===
    | Stack: word fd, long buf, long len
    move.l  #6, -(%sp)          | len
    pea     .Ldata(%pc)         | buf
    move.w  %d6, -(%sp)         | fd
    .word   0xFF40              | _WRITE
    lea     10(%sp), %sp        | clean 2+4+4 = 10
    cmp.l   #6, %d0
    beq     1f
    addq.l  #1, %d7
1:

    | === _SEEK(fd, 0, SEEK_SET) ===
    | Stack: word fd, long offset, word whence
    clr.w   -(%sp)              | whence = 0 (SEEK_SET)
    clr.l   -(%sp)              | offset = 0
    move.w  %d6, -(%sp)         | fd
    .word   0xFF42              | _SEEK
    addq.l  #8, %sp             | clean 2+4+2 = 8
    tst.l   %d0
    beq     1f
    addq.l  #1, %d7
1:

    | === _READ(fd, buf, 6) ===
    | Allocate 8 bytes on stack for read buffer
    sub.l   #8, %sp
    move.l  %sp, %a3            | a3 = read buffer

    move.l  #6, -(%sp)          | len
    move.l  %a3, -(%sp)         | buf
    move.w  %d6, -(%sp)         | fd
    .word   0xFF3F              | _READ
    lea     10(%sp), %sp        | clean 2+4+4 = 10
    cmp.l   #6, %d0
    beq     1f
    addq.l  #1, %d7
1:

    | Verify read data matches "Hello!"
    move.l  %sp, %a3            | a3 = buffer (sp points to it now)
    lea     .Ldata(%pc), %a2
    moveq   #5, %d0             | compare 6 bytes (counter 5..0)
.Lcmp:
    cmp.b   (%a2)+, (%a3)+
    bne     .Lcmp_fail
    dbf     %d0, .Lcmp
    bra     1f
.Lcmp_fail:
    addq.l  #1, %d7
1:
    addq.l  #8, %sp             | free read buffer

    | === _CLOSE(fd) ===
    | Stack: word fd
    move.w  %d6, -(%sp)
    .word   0xFF3E              | _CLOSE
    addq.l  #2, %sp
    tst.l   %d0
    beq     1f
    addq.l  #1, %d7
1:

    | === _OPEN("/tmp/h68k_test.dat", 0) — reopen for read ===
    | Stack: long path, word mode
    clr.w   -(%sp)              | mode = 0 (read)
    pea     .Lpath(%pc)
    .word   0xFF3D              | _OPEN
    addq.l  #6, %sp
    tst.l   %d0
    bpl     1f
    addq.l  #1, %d7
    bra     .Lskip_reopen
1:
    | Close the reopened handle
    move.w  %d0, -(%sp)
    .word   0xFF3E              | _CLOSE
    addq.l  #2, %sp
.Lskip_reopen:

    | === _DELETE("/tmp/h68k_test.dat") ===
    | Stack: long path
    pea     .Lpath(%pc)
    .word   0xFF41              | _DELETE
    addq.l  #4, %sp
    tst.l   %d0
    beq     1f
    addq.l  #1, %d7
1:

    | === _OPEN after delete — should fail (file gone) ===
    clr.w   -(%sp)
    pea     .Lpath(%pc)
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

.Lexit:
    move.w  %d7, -(%sp)
    .word   0xFF4C              | _EXIT2

    .even
.Lpath:
    .asciz  "/tmp/h68k_test.dat"
    .even
.Ldata:
    .ascii  "Hello!"
