/*
 * pico2.h — Official Raspberry Pi Pico 2 pin definitions
 *
 * RP2350, 4 MB flash, no SD card.
 * UART0: GP0 (TX) / GP1 (RX) — same as Pico 1.
 * No SPI peripherals used by PPAP.
 */

#ifndef PPAP_TARGET_PICO2_KERNEL_CORE_PICO2_H
#define PPAP_TARGET_PICO2_KERNEL_CORE_PICO2_H

#define PICO2_UART0_TX 0 /* GP0 */
#define PICO2_UART0_RX 1 /* GP1 */

/* On-board LED (active high) — useful for debug heartbeat */
#define PICO2_LED 25 /* GP25 */

#endif /* PPAP_TARGET_PICO2_KERNEL_CORE_PICO2_H */
