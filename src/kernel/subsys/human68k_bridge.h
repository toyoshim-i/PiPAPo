/*
 * human68k_bridge.h — Human68k DOS call bridge
 *
 * Translates Human68k F-line DOS calls ($FFxx) into PPAP syscalls.
 * Called from m68k_fline_dispatch() when current->subsys == SUBSYS_HUMAN68K.
 */

#ifndef PPAP_SUBSYS_HUMAN68K_BRIDGE_H
#define PPAP_SUBSYS_HUMAN68K_BRIDGE_H

#include <stdint.h>

/*
 * human68k_dos_dispatch — handle one Human68k DOS call.
 *
 * @regs:   saved register frame (d0-d7 at [0..7], a0-a6 at [8..14],
 *          then SR(2)+PC(4) at byte offset 60).
 * @usp:    user stack pointer at time of F-line exception.
 * @opcode: the 16-bit F-line opcode ($FFxx).
 *
 * Returns:  1 = process exited (caller should schedule next)
 *           2 = DOS call handled, return to user mode
 *          -1 = unhandled DOS call (caller should crash/SIGILL)
 */
int human68k_dos_dispatch(uint32_t *regs, uint32_t usp, uint16_t opcode);

#endif /* PPAP_SUBSYS_HUMAN68K_BRIDGE_H */
