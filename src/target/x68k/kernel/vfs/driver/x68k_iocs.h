/*
 * x68k_iocs.h — IOCS serialization helpers
 *
 * The X68000 IPL ROM's IOCS handlers (TRAP #15) are non-reentrant: e.g.
 * _B_PUTC temporarily lowers IPL to allow VSYNC sync, so if a Timer-C
 * tick fires during an IOCS call and the ISR re-enters IOCS via the
 * scheduler, the shared 0x000400-0x0007FF work area is corrupted.
 *
 * Process-context callers enter the IOCS mutex before issuing a trap.
 * Callers that can safely run the trap with interrupts masked also wrap the
 * trap pair with ipl7_save / ipl7_restore.
 */

#ifndef PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_X68K_IOCS_H
#define PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_X68K_IOCS_H

#include <stdint.h>

void x68k_iocs_init(void);
void x68k_iocs_enter(void);
void x68k_iocs_exit(void);
int x68k_iocs_held_by_current(void);
int x68k_iocs_try_enter(void);

static inline uint16_t ipl7_save(void) {
  uint16_t sr;
  asm volatile(
      "move.w %%sr,%0\n\t"
      "ori.w #0x0700,%%sr"
      : "=d"(sr)
      :
      : "memory");
  return sr;
}

static inline void ipl7_restore(uint16_t sr) {
  asm volatile("move.w %0,%%sr" : : "d"(sr) : "memory");
}

#endif /* PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_X68K_IOCS_H */
