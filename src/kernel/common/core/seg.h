/*
 * seg.h — Kernel module segment manager
 *
 * On i16, each kernel module lives in its own 64 KB segment.
 * seg_table[] holds each module's paragraph address (linear >> 4).
 * Defined in arch/i16/kernel/core/seg.c, unused on 32-bit targets.
 */

#ifndef PPAP_KERNEL_COMMON_CORE_SEG_H
#define PPAP_KERNEL_COMMON_CORE_SEG_H

#include <stdint.h>

/* Module IDs */
#define MOD_ID_CORE 0
#define MOD_ID_VFS 1
#define MOD_ID_MAX 8

extern uint16_t seg_table[MOD_ID_MAX];

static inline void seg_register(uint8_t id, uint16_t seg) {
  seg_table[id] = seg;
}

static inline uint16_t seg_get(uint8_t id) { return seg_table[id]; }

#endif /* PPAP_KERNEL_COMMON_CORE_SEG_H */
