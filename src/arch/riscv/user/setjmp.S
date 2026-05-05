// setjmp.S — RISC-V (RV32) setjmp / longjmp
//
// Layout in jmp_buf (56 bytes used out of 64):
//   0..47   s0-s11  (12 longs)
//  48       sp
//  52       ra

.section .text.setjmp, "ax"
.global setjmp
setjmp:
    sw s0,  0(a0)
    sw s1,  4(a0)
    sw s2,  8(a0)
    sw s3, 12(a0)
    sw s4, 16(a0)
    sw s5, 20(a0)
    sw s6, 24(a0)
    sw s7, 28(a0)
    sw s8, 32(a0)
    sw s9, 36(a0)
    sw s10, 40(a0)
    sw s11, 44(a0)
    sw sp, 48(a0)
    sw ra, 52(a0)
    li a0, 0
    ret

.section .text.longjmp, "ax"
.global longjmp
longjmp:
    lw s0,  0(a0)
    lw s1,  4(a0)
    lw s2,  8(a0)
    lw s3, 12(a0)
    lw s4, 16(a0)
    lw s5, 20(a0)
    lw s6, 24(a0)
    lw s7, 28(a0)
    lw s8, 32(a0)
    lw s9, 36(a0)
    lw s10, 40(a0)
    lw s11, 44(a0)
    lw sp, 48(a0)
    lw ra, 52(a0)
    mv a0, a1
    bnez a0, 1f
    li a0, 1                  // longjmp(env, 0) -> 1
1:  ret
