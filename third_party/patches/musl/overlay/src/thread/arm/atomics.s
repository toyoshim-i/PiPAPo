/* PicoPiAndPortable — Cortex-M0+ (ARMv6-M, Thumb-1) atomics.
 *
 * Replaces upstream musl ARM atomics which use:
 *   - conditional execution (streq) — not available in Thumb-1
 *   - LDREX/STREX  — not available on ARMv6-M
 *   - CP15 MRC     — not available on Cortex-M profile
 *   - Linux kuser helpers at 0xFFFF0FC0 — no Linux kernel
 *
 * Single-core M0+ CAS uses interrupt masking (CPSID/CPSIE) for
 * atomicity.  Barrier uses DMB which is available on all ARMv6-M.
 * Thread pointer comes from a static variable __ppap_tp.
 */

.syntax unified
.thumb
.cpu cortex-m0plus
.text

/* ------------------------------------------------------------------ */
/* Compare-and-swap                                                   */
/*   r0 = expected old value                                          */
/*   r1 = desired new value                                           */
/*   r2 = pointer to int                                              */
/*   Returns r0 = 0 on success, non-zero on failure                   */
/* ------------------------------------------------------------------ */
.global __a_cas_dummy
.hidden __a_cas_dummy
.type __a_cas_dummy,%function
__a_cas_dummy:
	push {r4}
	mrs r4, primask       /* save interrupt state */
	cpsid i               /* begin critical section */
	mov r3, r0            /* r3 = expected */
	ldr r0, [r2]          /* r0 = *ptr (actual) */
	cmp r0, r3
	bne 1f
	str r1, [r2]          /* *ptr = desired */
1:	subs r0, r3, r0       /* 0 if matched, non-zero otherwise */
	msr primask, r4       /* end critical section */
	pop {r4}
	bx lr

/* ------------------------------------------------------------------ */
/* Memory barrier — DMB is available on all ARMv6-M cores             */
/* ------------------------------------------------------------------ */
.global __a_barrier_dummy
.hidden __a_barrier_dummy
.type __a_barrier_dummy,%function
__a_barrier_dummy:
	dmb
	bx lr

/* ------------------------------------------------------------------ */
/* Thread pointer accessor — replaces __a_gettp_cp15 (no CP15 on M0+)*/
/* ------------------------------------------------------------------ */
.global __a_gettp_dummy
.hidden __a_gettp_dummy
.type __a_gettp_dummy,%function
__a_gettp_dummy:
	ldr r0, 1f
	ldr r0, [r0]
2:	bx lr
	.align 2
1:	.word __ppap_tp

/* ------------------------------------------------------------------ */
/* Function-pointer dispatch table (in .data so __set_thread_area can */
/* update them, though PPAP never does runtime dispatch)              */
/* ------------------------------------------------------------------ */
.data
.align 2

.global __a_barrier_ptr
.hidden __a_barrier_ptr
__a_barrier_ptr:
	.word __a_barrier_dummy

.global __a_cas_ptr
.hidden __a_cas_ptr
__a_cas_ptr:
	.word __a_cas_dummy

.global __a_gettp_ptr
.hidden __a_gettp_ptr
__a_gettp_ptr:
	.word __a_gettp_dummy

/* Thread pointer storage — written by __set_thread_area(), read by
 * __get_tp() in pthread_arch.h and __aeabi_read_tp.                  */
.global __ppap_tp
.hidden __ppap_tp
__ppap_tp:
	.word 0
