/*
 * atomic_arch.h — m68000-compatible atomics for PPAP
 *
 * The upstream musl m68k atomic_arch.h uses cas.l (m68020+).
 * PPAP targets m68000 (no cas instruction) and is single-core,
 * so we use non-atomic load/store with compiler barriers.
 */

#define a_cas a_cas
static inline int a_cas(volatile int *p, int t, int s)
{
	int old;
	__asm__ __volatile__ ("" ::: "memory");
	old = *p;
	if (old == t) *p = s;
	__asm__ __volatile__ ("" ::: "memory");
	return old;
}
