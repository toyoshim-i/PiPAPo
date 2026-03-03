/*
 * pico1.h — Official Raspberry Pi Pico pin definitions
 *
 * RP2040, 2 MB flash, no SD card.
 * UART0: GP0 (TX) / GP1 (RX) — same as PicoCalc.
 * No SPI peripherals used by PPAP.
 */

#ifndef PPAP_TARGET_PICO1_H
#define PPAP_TARGET_PICO1_H

#define PICO1_UART0_TX      0    /* GP0 */
#define PICO1_UART0_RX      1    /* GP1 */

/* On-board LED (active high) — useful for debug heartbeat */
#define PICO1_LED            25   /* GP25 */

#endif /* PPAP_TARGET_PICO1_H */
