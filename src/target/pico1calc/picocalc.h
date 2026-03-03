/*
 * picocalc.h — ClockworkPi PicoCalc board pin definitions
 *
 * The PicoCalc carries an RP2040 with a full-size SD card slot wired to
 * SPI0.  This header centralises all board-specific GPIO assignments so
 * that drivers (spi.c, sd.c) remain board-agnostic.
 *
 * Pin assignments (from PicoCalc schematic):
 *   SPI0_RX  (MISO): GP16   FUNCSEL 1
 *   SPI0_CSn (CS)  : GP17   FUNCSEL 5 (SIO — manual GPIO)
 *   SPI0_SCK       : GP18   FUNCSEL 1
 *   SPI0_TX  (MOSI): GP19   FUNCSEL 1
 *   SD_CD          : GP22   FUNCSEL 5 (SIO — input, active low)
 */

#ifndef PPAP_TARGET_PICOCALC_H
#define PPAP_TARGET_PICOCALC_H

/* ── SPI0 / SD card pins ──────────────────────────────────────────────────── */

#define PICOCALC_SPI0_RX     16   /* MISO — GP16, FUNCSEL 1 */
#define PICOCALC_SPI0_CS     17   /* CS   — GP17, manual GPIO */
#define PICOCALC_SPI0_SCK    18   /* SCK  — GP18, FUNCSEL 1 */
#define PICOCALC_SPI0_TX     19   /* MOSI — GP19, FUNCSEL 1 */
#define PICOCALC_SD_CD       22   /* Card detect — GP22, active low */

/* ── GPIO function select values ──────────────────────────────────────────── */

#define GPIO_FUNC_SPI        1   /* SPI function (FUNCSEL = 1) */
#define GPIO_FUNC_SIO        5   /* SIO — manual GPIO (FUNCSEL = 5) */

#endif /* PPAP_TARGET_PICOCALC_H */
