/* PicoPiAndPortable — Cortex-M0+ (ARMv6-M) thread pointer.
 *
 * No CP15 or Linux kuser helpers available.  Single-threaded processes
 * use a static variable set by __set_thread_area() (generic C version
 * falls back to SYS_set_thread_area syscall).
 *
 * __ppap_tp is defined in atomics.s alongside other ARM globals. */

#include "libc.h"

extern hidden uintptr_t __ppap_tp;

static inline uintptr_t __get_tp()
{
	return __ppap_tp;
}

#define TLS_ABOVE_TP
#define GAP_ABOVE_TP 8

#define MC_PC arm_pc
