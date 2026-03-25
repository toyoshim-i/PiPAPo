/*
 * seg.h — Kernel module segment manager
 *
 * On i16, each kernel module lives in its own 64 KB segment (CS=DS).
 * The segment manager tracks each module's segment base (paragraph
 * address = linear_addr >> 4).
 *
 * On 32-bit platforms, this is a no-op — all modules share a flat
 * address space.
 */

#ifndef PPAP_KERNEL_COMMON_SEG_H
#define PPAP_KERNEL_COMMON_SEG_H

#include <stdint.h>

/* Module IDs — add new modules here */
#define MOD_ID_CORE  0
#define MOD_ID_VFS   1
#define MOD_ID_EXEC  2
#define MOD_ID_MAX   8  /* max modules */

#ifdef __ia16__

/* Register a module's segment base (paragraph address).
 * Called by stage2 for core, and by core boot for others. */
void seg_register(uint8_t mod_id, uint16_t segment);

/* Look up a module's segment base. */
uint16_t seg_get(uint8_t mod_id);

#else

/* No-op on 32-bit platforms */
static inline void seg_register(uint8_t mod_id, uint16_t segment) {
  (void)mod_id; (void)segment;
}
static inline uint16_t seg_get(uint8_t mod_id) {
  (void)mod_id; return 0;
}

#endif /* __ia16__ */

#endif /* PPAP_KERNEL_COMMON_SEG_H */
