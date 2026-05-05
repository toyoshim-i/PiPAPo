| setjmp.S — m68k setjmp / longjmp
|
| Layout in jmp_buf (44 bytes used out of 64):
|   0..23  d2-d7  (6 longs)
|  24..43  a2-a6  (5 longs)
|  44      sp
|  48      pc

.section .text.setjmp, "ax"
.global setjmp
setjmp:
    move.l 4(%sp), %a0           | env pointer (passed on stack)
    movem.l %d2-%d7/%a2-%a6, (%a0)
    move.l %sp, 44(%a0)          | save sp
    move.l (%sp), 48(%a0)        | save return PC
    moveq #0, %d0
    rts

.section .text.longjmp, "ax"
.global longjmp
longjmp:
    move.l 4(%sp), %a0           | env
    move.l 8(%sp), %d0           | val
    movem.l (%a0), %d2-%d7/%a2-%a6
    move.l 44(%a0), %sp          | restore sp
    move.l 48(%a0), -(%sp)       | push saved PC as new return
    tst.l %d0
    bne.s 1f
    moveq #1, %d0                | longjmp(env, 0) -> 1
1:  rts
