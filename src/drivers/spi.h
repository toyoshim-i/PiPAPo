/*
 * spi.h — SPI0 driver interface (RP2040 PL022)
 *
 * Drives the PL022 (ARM PrimeCell SSP) SPI0 controller on the RP2040.
 * Pin assignments come from src/target/pico1calc/pico1calc.h.
 *
 * Usage:
 *   spi_init(400000)          — initialise SPI0 at 400 kHz (SD card init)
 *   spi_set_baud(25000000)    — switch to 25 MHz (after SD card is ready)
 *   spi_xfer(0xFF)            — send one byte, receive one byte
 *   spi_xfer_block(...)       — bulk transfer
 *   sd_cs_low() / sd_cs_high()— manual chip-select for SD card
 *   spi_card_detect()         — check SD card presence (GP22, active low)
 */

#ifndef PPAP_DRIVERS_SPI_H
#define PPAP_DRIVERS_SPI_H

#include <stdint.h>
#include <stddef.h>

/* Initialise SPI0 at the specified baud rate.
 * Configures GPIO pins, resets the peripheral, and enables the controller.
 * Call once from kmain() after clock_init_pll(). */
void spi_init(uint32_t baud_hz);

/* Change the SPI clock rate without re-initialising the peripheral.
 * Used to switch from 400 kHz (SD init) to 25 MHz (normal operation). */
void spi_set_baud(uint32_t baud_hz);

/* Transfer one byte: sends `tx`, returns the received byte. */
uint8_t spi_xfer(uint8_t tx);

/* Block transfer: send `len` bytes from `tx_buf` while receiving into `rx_buf`.
 * Either `tx_buf` or `rx_buf` may be NULL:
 *   tx_buf==NULL → sends 0xFF for each byte (receive-only)
 *   rx_buf==NULL → discards received bytes (send-only) */
void spi_xfer_block(const uint8_t *tx_buf, uint8_t *rx_buf, size_t len);

/* Assert SD card chip select (GP17 low). */
void sd_cs_low(void);

/* De-assert SD card chip select (GP17 high) and send one dummy byte. */
void sd_cs_high(void);

/* Check SD card presence via the card-detect pin (GP22).
 * Returns 1 if a card is inserted, 0 if absent. */
int spi_card_detect(void);

#endif /* PPAP_DRIVERS_SPI_H */
