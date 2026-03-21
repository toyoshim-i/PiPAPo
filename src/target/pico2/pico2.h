/*
 * pico2.h — Official Raspberry Pi Pico 2 pin definitions
 *
 * RP2350, 4 MB flash, no SD card.
 * UART0: GP0 (TX) / GP1 (RX) — same as Pico 1.
 * No SPI peripherals used by PPAP.
 */

#ifndef PPAP_TARGET_PICO2_PICO2_H
#define PPAP_TARGET_PICO2_PICO2_H

#define PICO2_UART0_TX      0    /* GP0 */
#define PICO2_UART0_RX      1    /* GP1 */

/* On-board LED (active high) — useful for debug heartbeat */
#define PICO2_LED            25   /* GP25 */

/* ── RP2350 TrustZone address aliases ─────────────────────────────────────
 *
 * The IDAU on RP2350 uses bit 28 to distinguish Secure from Non-Secure:
 *
 *   Secure      Non-Secure    Region
 *   0x10xxxxxx  0x00xxxxxx    Flash / XIP
 *   0x20xxxxxx  0x30xxxxxx    SRAM
 *   0x40xxxxxx  0x50xxxxxx    APB peripherals
 *
 * XOR with 0x10000000 toggles between the two aliases for the same
 * physical memory.  Kernel code always uses the Secure alias; NS user
 * processes will use the Non-Secure alias.
 * ────────────────────────────────────────────────────────────────────────── */
#define RP2350_NS_BIT         0x10000000u
#define RP2350_ADDR_S_TO_NS(a)  ((uint32_t)(a) ^ RP2350_NS_BIT)
#define RP2350_ADDR_NS_TO_S(a)  ((uint32_t)(a) ^ RP2350_NS_BIT)

#endif /* PPAP_TARGET_PICO2_PICO2_H */
