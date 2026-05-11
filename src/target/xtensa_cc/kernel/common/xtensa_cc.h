/*
 * xtensa_cc.h — M5Stack CardComputer (ESP32-S3 / STAMP S3) definitions
 *
 * Pin assignments and hardware constants for the CardComputer target.
 * Display, keyboard, and SD card pins are defined here for future phases.
 *
 * Lives under kernel/common/ — every symbol below is a board-description
 * constant (pin numbers, clock fallback) consumed by VFS-side
 * peripheral drivers; placing it under kernel/core/ would force VFS
 * code to reach across the module boundary.
 */

#ifndef PPAP_TARGET_XTENSA_CC_KERNEL_COMMON_XTENSA_CC_H
#define PPAP_TARGET_XTENSA_CC_KERNEL_COMMON_XTENSA_CC_H

/* ── System clock ──────────────────────────────────────────────────────── */

#ifndef PPAP_SYS_HZ
#define PPAP_SYS_HZ 240000000u  /* ESP32-S3 default: 240 MHz */
#endif

/* ── UART (USB CDC-ACM, directly routed to internal USB) ───────────── */
/* ESP-IDF configures UART0 for console output by default.
 * On CardComputer, USB-C is wired to the ESP32-S3 native USB peripheral.
 * For CC-1 we use UART0 (ESP-IDF default console). */

/* ── ST7789V2 Display (SPI2) ──────────────────────────────────────────── */

#define DISPLAY_MOSI_PIN    35
#define DISPLAY_SCK_PIN     36
#define DISPLAY_CS_PIN      37
#define DISPLAY_DC_PIN      34
#define DISPLAY_RST_PIN     33
#define DISPLAY_BL_PIN      38
#define DISPLAY_WIDTH       240
#define DISPLAY_HEIGHT      135

/* ── Keyboard (GPIO matrix: 7 rows × 8 columns) ──────────────────────── */

/* Row pins (directly from M5Stack Arduino library) */
#define KBD_ROW_PINS        { 8, 9, 11, 13, 15, 3, 4 }
#define KBD_COL_PINS        { 18, 17, 16, 7, 6, 5, 10, 0 }
#define KBD_NUM_ROWS        7
#define KBD_NUM_COLS        8

/* ── microSD (HSPI) ──────────────────────────────────────────────────── */

#define SD_MISO_PIN         39
#define SD_MOSI_PIN         14
#define SD_SCK_PIN          40
#define SD_CS_PIN           12

/* ── Speaker (I2S, NS4168) ───────────────────────────────────────────── */

#define SPK_BCLK_PIN        41
#define SPK_LRCK_PIN        43
#define SPK_DIN_PIN         42

/* ── IR Transmitter ──────────────────────────────────────────────────── */

#define IR_TX_PIN           44

#endif /* PPAP_TARGET_XTENSA_CC_KERNEL_COMMON_XTENSA_CC_H */
