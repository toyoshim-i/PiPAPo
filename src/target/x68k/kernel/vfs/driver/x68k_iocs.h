/*
 * x68k_iocs.h — IOCS serialization helpers
 *
 * The X68000 IPL ROM's IOCS handlers (TRAP #15) are non-reentrant: e.g.
 * _B_PUTC temporarily lowers IPL to allow VSYNC sync, so if a Timer-C
 * tick fires during an IOCS call and the ISR re-enters IOCS via the
 * scheduler, the shared 0x000400-0x0007FF work area is corrupted.
 *
 * Process-context callers enter the IOCS mutex before issuing a trap.  The
 * trap itself should run with the ROM's required interrupts enabled, but with
 * PPAP's Timer-C scheduler source masked so it cannot re-enter IOCS.
 */

#ifndef PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_X68K_IOCS_H
#define PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_X68K_IOCS_H

#include <stdint.h>

typedef uint32_t x68k_iocs_irq_state_t;

void x68k_iocs_init(void);
void x68k_iocs_enter(void);
void x68k_iocs_exit(void);
int x68k_iocs_held_by_current(void);
int x68k_iocs_try_enter(void);
x68k_iocs_irq_state_t x68k_iocs_irq_begin(void);
void x68k_iocs_irq_end(x68k_iocs_irq_state_t state);

#endif /* PPAP_TARGET_X68K_KERNEL_VFS_DRIVER_X68K_IOCS_H */
