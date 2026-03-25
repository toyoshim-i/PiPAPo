/*
 * seg.c — Kernel module segment manager (i16 only)
 *
 * Maintains a table of segment bases indexed by module ID.
 * The table lives in the core module's data segment, so it's
 * accessible via near pointers from core's stubs.
 */

#ifdef __IA16__

#include "seg.h"

static uint16_t seg_table[MOD_ID_MAX];

void seg_register(uint8_t mod_id, uint16_t segment) {
  if (mod_id < MOD_ID_MAX)
    seg_table[mod_id] = segment;
}

uint16_t seg_get(uint8_t mod_id) {
  if (mod_id < MOD_ID_MAX)
    return seg_table[mod_id];
  return 0;
}

#endif /* __IA16__ */
