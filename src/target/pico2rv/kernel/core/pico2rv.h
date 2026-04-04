/*
 * pico2rv.h — Official Raspberry Pi Pico 2 pin definitions (RISC-V mode)
 *
 * RP2350 in Hazard3 RISC-V mode, 4 MB flash, no SD card.
 * Peripheral pins are identical to pico2 (ARM mode).
 * UART0: GP0 (TX) / GP1 (RX) — same as Pico 1.
 */

#ifndef PPAP_TARGET_PICO2RV_H
#define PPAP_TARGET_PICO2RV_H

#define PICO2RV_UART0_TX    0    /* GP0 */
#define PICO2RV_UART0_RX    1    /* GP1 */

/* On-board LED (active high) — useful for debug heartbeat */
#define PICO2RV_LED          25   /* GP25 */

#endif /* PPAP_TARGET_PICO2RV_H */
