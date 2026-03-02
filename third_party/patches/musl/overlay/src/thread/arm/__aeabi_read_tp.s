/* PicoPiAndPortable — __aeabi_read_tp for Cortex-M0+.
 *
 * Returns the thread pointer from the static __ppap_tp variable
 * instead of calling through __a_gettp_ptr (no runtime dispatch
 * needed on single-core M0+). */

.syntax unified
.thumb
.cpu cortex-m0plus

.global __aeabi_read_tp
.type __aeabi_read_tp,%function
__aeabi_read_tp:
	ldr r0, 1f
	ldr r0, [r0]
	bx lr
	.align 2
1:	.word __ppap_tp
