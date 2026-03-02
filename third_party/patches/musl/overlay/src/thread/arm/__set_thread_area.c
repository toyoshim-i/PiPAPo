/* PicoPiAndPortable — __set_thread_area for Cortex-M0+.
 *
 * Upstream ARM version does runtime CPU capability detection and
 * updates atomics/barrier/gettp function pointers.  PPAP runs on
 * single-core Cortex-M0+ with fixed implementations, so we just
 * store the thread pointer. */

#include "pthread_impl.h"

extern hidden uintptr_t __ppap_tp;

int __set_thread_area(void *p)
{
	__ppap_tp = (uintptr_t)p;
	return 0;
}
