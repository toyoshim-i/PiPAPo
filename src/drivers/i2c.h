/*
 * i2c.h — I2C1 master driver interface (RP2040 DW_apb_i2c)
 *
 * Drives the Synopsys DesignWare I2C1 controller on the RP2040 to
 * communicate with the STM32 keyboard controller on the PicoCalc board.
 * Pin assignments come from src/target/pico1calc/pico1calc.h.
 *
 * Usage:
 *   i2c_init()                          — initialise I2C1 at 10 kHz
 *   i2c_read_reg(0x1F, 0x04, &val, 1)  — read 1 byte from register 0x04
 *   i2c_write_reg(0x1F, 0x05, &val, 1) — write 1 byte to register 0x05
 */

#ifndef PPAP_DRIVERS_I2C_H
#define PPAP_DRIVERS_I2C_H

#include <stdint.h>
#include <stddef.h>

/* Initialise I2C1 at 10 kHz (standard mode, master).
 * Configures GPIO pins GP6 (SDA) and GP7 (SCL), resets the peripheral,
 * and sets the SCL timing for 10 kHz at 133 MHz peri clock.
 * Call once from target_early_init() after clock_init_pll(). */
void i2c_init(void);

/* Read `len` bytes from register `reg` on slave `addr` into `buf`.
 * Performs: START → write reg → RESTART → read len bytes → STOP.
 * Returns 0 on success, -1 on NAK/timeout. */
int i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);

/* Write `len` bytes from `buf` to register `reg` on slave `addr`.
 * Performs: START → write reg → write data[0..len-1] → STOP.
 * Returns 0 on success, -1 on NAK/timeout. */
int i2c_write_reg(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len);

#endif /* PPAP_DRIVERS_I2C_H */
