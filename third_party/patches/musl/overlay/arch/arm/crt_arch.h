/* PicoPiAndPortable — Cortex-M0+ (Thumb-1) CRT startup.
 *
 * Replaces upstream ARM crt_arch.h which uses:
 *   - mov fp, #0     — hi-reg immediate not available in Thumb-1
 *   - mov lr, #0     — hi-reg immediate not available in Thumb-1
 *   - and ip, a1, #-16 — bitwise AND with immediate not in Thumb-1
 */
__asm__(
".syntax unified \n"
".thumb \n"
".text \n"
".global " START " \n"
".type " START ",%function \n"
".thumb_func \n"
START ": \n"
"	movs r3, #0 \n"        /* use low reg to zero hi regs */
"	mov fp, r3 \n"          /* clear frame pointer */
"	mov lr, r3 \n"          /* clear link register */
"	ldr r1, 1f \n"          /* r1 = offset to _DYNAMIC */
"	add r1, r1, pc \n"      /* r1 += PC → address of _DYNAMIC */
"	mov r0, sp \n"          /* r0 = original stack pointer (arg) */
"2:	mov r2, sp \n"          /* label 2 must be here for _DYNAMIC-2b */
"	movs r3, #0xf \n"
"	bics r2, r3 \n"         /* r2 = sp & ~0xf (16-byte aligned) */
"	mov sp, r2 \n"          /* set aligned stack pointer */
"	bl " START "_c \n"      /* _start_c(original_sp_in_r0) */
".weak _DYNAMIC \n"
".hidden _DYNAMIC \n"
".align 2 \n"
"1:	.word _DYNAMIC-2b \n"
);
