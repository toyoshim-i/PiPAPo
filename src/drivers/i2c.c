/*
 * i2c.c — I2C1 master driver for RP2040 (Synopsys DesignWare DW_apb_i2c)
 *
 * Drives the I2C1 controller in master mode to communicate with the STM32
 * keyboard controller on the PicoCalc board.  Pin assignments are defined
 * in src/target/pico1calc/pico1calc.h.
 *
 * Bus speed: 10 kHz (critical for STM32 stability per PicoCalc hardware spec).
 * External 4.7kΩ pull-ups on GP6/GP7 (mainboard V2.0).
 *
 * SCL timing at 133 MHz peri clock:
 *   Period = 133 000 000 / 10 000 = 13 300 cycles
 *   HCNT = LCNT = 6650 (equal duty cycle)
 */

#include "i2c.h"
#include "../target/pico1calc/pico1calc.h"
#include "../hw/rp2040.h"
#include "config.h"
#include <stdint.h>

/* ── I2C1 (DW_apb_i2c) — base 0x40048000 ────────────────────────────────── */

#define I2C1_BASE  0x40048000u

#define IC_CON              REG(I2C1_BASE + 0x00u)
#define IC_TAR              REG(I2C1_BASE + 0x04u)
#define IC_DATA_CMD         REG(I2C1_BASE + 0x10u)
#define IC_SS_SCL_HCNT      REG(I2C1_BASE + 0x14u)
#define IC_SS_SCL_LCNT      REG(I2C1_BASE + 0x18u)
#define IC_RAW_INTR_STAT    REG(I2C1_BASE + 0x34u)
#define IC_CLR_INTR         REG(I2C1_BASE + 0x40u)
#define IC_CLR_TX_ABRT      REG(I2C1_BASE + 0x54u)
#define IC_ENABLE           REG(I2C1_BASE + 0x6Cu)
#define IC_STATUS           REG(I2C1_BASE + 0x70u)
#define IC_TXFLR            REG(I2C1_BASE + 0x74u)
#define IC_RXFLR            REG(I2C1_BASE + 0x78u)
#define IC_TX_ABRT_SOURCE   REG(I2C1_BASE + 0x80u)
#define IC_ENABLE_STATUS    REG(I2C1_BASE + 0x9Cu)

/* IC_CON fields */
#define CON_MASTER_MODE     (1u << 0)   /* master enable                    */
#define CON_SPEED_STD       (1u << 1)   /* standard mode (≤100 kHz) [2:1]=01*/
#define CON_IC_RESTART_EN   (1u << 5)   /* RESTART condition enable         */
#define CON_IC_SLAVE_DISABLE (1u << 6)  /* disable slave mode               */

/* IC_DATA_CMD fields */
#define DATA_CMD_READ       (1u << 8)   /* 1=read, 0=write                  */
#define DATA_CMD_STOP       (1u << 9)   /* issue STOP after this byte       */
#define DATA_CMD_RESTART    (1u << 10)  /* issue RESTART before this byte   */

/* IC_STATUS fields */
#define STATUS_TFNF         (1u << 1)   /* TX FIFO not full                 */
#define STATUS_TFE          (1u << 2)   /* TX FIFO empty                    */
#define STATUS_RFNE         (1u << 3)   /* RX FIFO not empty                */
#define STATUS_ACTIVITY     (1u << 0)   /* controller activity              */
#define STATUS_MST_ACTIVITY (1u << 5)   /* master activity                  */

/* IC_RAW_INTR_STAT fields */
#define RAW_INTR_TX_ABRT    (1u << 6)   /* TX abort                         */

/* ── GPIO — IO_BANK0 0x40014000 ──────────────────────────────────────────── */

#define IO_BANK0_BASE   0x40014000u
#define GPIO_CTRL(n)    REG(IO_BANK0_BASE + (n) * 8u + 4u)

/* ── GPIO — PADS_BANK0 0x4001C000 ────────────────────────────────────────── */

#define PADS_BANK0_BASE 0x4001C000u
#define PAD_GPIO(n)     REG(PADS_BANK0_BASE + 4u + (n) * 4u)

/* IE=1, PUE=1, DRIVE=4mA — same as PAD_SPI_IN.
 * External 4.7kΩ pull-ups exist; internal PUE is harmless in parallel. */
#define PAD_I2C         (0x58u)

/* ── SCL timing for 10 kHz @ 133 MHz ─────────────────────────────────────── */

#define I2C_SCL_HCNT    6650u
#define I2C_SCL_LCNT    6650u

/* ── Timeout ─────────────────────────────────────────────────────────────── */

/* ~100 ms at 133 MHz.  Each loop iteration is several cycles; this is a
 * conservative upper bound, not cycle-accurate.  Sufficient for detecting
 * a stuck bus or missing slave. */
#define I2C_TIMEOUT     200000u

/* ── Internal state ──────────────────────────────────────────────────────── */

static uint8_t current_tar = 0xFF;  /* cached target address (0xFF = unset) */

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void i2c_disable(void)
{
    IC_ENABLE = 0;
    /* Wait for controller to become disabled */
    uint32_t timeout = I2C_TIMEOUT;
    while ((IC_ENABLE_STATUS & 1u) && --timeout)
        ;
}

static void i2c_enable(void)
{
    IC_ENABLE = 1;
}

/* Set target address.  IC_TAR can only be changed while disabled. */
static void i2c_set_tar(uint8_t addr)
{
    if (addr == current_tar)
        return;
    i2c_disable();
    IC_TAR = addr;
    current_tar = addr;
    i2c_enable();
}

/* Clear any pending TX abort. */
static void i2c_clear_abort(void)
{
    if (IC_RAW_INTR_STAT & RAW_INTR_TX_ABRT)
        (void)IC_CLR_TX_ABRT;
}

/* Wait for TX FIFO to have space.  Returns 0 on success, -1 on abort/timeout. */
static int i2c_wait_tx_ready(void)
{
    uint32_t timeout = I2C_TIMEOUT;
    while (!(IC_STATUS & STATUS_TFNF)) {
        if (IC_RAW_INTR_STAT & RAW_INTR_TX_ABRT)
            return -1;
        if (--timeout == 0)
            return -1;
    }
    return 0;
}

/* Wait for RX FIFO to have data.  Returns 0 on success, -1 on abort/timeout. */
static int i2c_wait_rx_ready(void)
{
    uint32_t timeout = I2C_TIMEOUT;
    while (!(IC_STATUS & STATUS_RFNE)) {
        if (IC_RAW_INTR_STAT & RAW_INTR_TX_ABRT)
            return -1;
        if (--timeout == 0)
            return -1;
    }
    return 0;
}

/* Wait for all TX data to be sent and bus to go idle. */
static int i2c_wait_idle(void)
{
    uint32_t timeout = I2C_TIMEOUT;
    while (IC_STATUS & STATUS_MST_ACTIVITY) {
        if (IC_RAW_INTR_STAT & RAW_INTR_TX_ABRT)
            return -1;
        if (--timeout == 0)
            return -1;
    }
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void i2c_init(void)
{
    /* 1. Full reset cycle — assert then de-assert (same rationale as SPI0/SPI1:
     *    the UF2 bootloader may leave the peripheral in an unknown state). */
    RESETS_RESET_SET = RESET_I2C1;
    RESETS_RESET_CLR = RESET_I2C1;
    while (!(RESETS_RESET_DONE & RESET_I2C1))
        ;

    /* 2. Configure GPIO pads for I2C (IE=1, PUE=1, 4mA) */
    PAD_GPIO(PICOCALC_I2C1_SDA) = PAD_I2C;
    PAD_GPIO(PICOCALC_I2C1_SCL) = PAD_I2C;

    /* 3. Set GPIO function select to I2C (FUNCSEL = 3) */
    GPIO_CTRL(PICOCALC_I2C1_SDA) = GPIO_FUNC_I2C;
    GPIO_CTRL(PICOCALC_I2C1_SCL) = GPIO_FUNC_I2C;

    /* 4. Disable controller while configuring */
    i2c_disable();

    /* 5. Configure: master mode, standard speed, 7-bit addr, restart enable,
     *    slave mode disabled */
    IC_CON = CON_MASTER_MODE | CON_SPEED_STD
           | CON_IC_RESTART_EN | CON_IC_SLAVE_DISABLE;

    /* 6. SCL timing for 10 kHz @ 133 MHz */
    IC_SS_SCL_HCNT = I2C_SCL_HCNT;
    IC_SS_SCL_LCNT = I2C_SCL_LCNT;

    /* 7. Clear any stale interrupts */
    (void)IC_CLR_INTR;

    /* 8. Enable controller */
    current_tar = 0xFF;
    i2c_enable();
}

int i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    if (len == 0)
        return 0;

    i2c_clear_abort();
    i2c_set_tar(addr);

    /* Write the register address byte */
    if (i2c_wait_tx_ready() < 0)
        goto fail;
    IC_DATA_CMD = reg;

    /* Issue read commands with RESTART on the first byte, STOP on the last */
    for (size_t i = 0; i < len; i++) {
        if (i2c_wait_tx_ready() < 0)
            goto fail;

        uint32_t cmd = DATA_CMD_READ;
        if (i == 0)
            cmd |= DATA_CMD_RESTART;
        if (i == len - 1)
            cmd |= DATA_CMD_STOP;
        IC_DATA_CMD = cmd;
    }

    /* Collect response bytes */
    for (size_t i = 0; i < len; i++) {
        if (i2c_wait_rx_ready() < 0)
            goto fail;
        buf[i] = (uint8_t)(IC_DATA_CMD & 0xFFu);
    }

    i2c_wait_idle();
    return 0;

fail:
    i2c_clear_abort();
    return -1;
}

int i2c_write_reg(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len)
{
    i2c_clear_abort();
    i2c_set_tar(addr);

    /* Write the register address byte.
     * If len == 0, this is the only byte — issue STOP. */
    if (i2c_wait_tx_ready() < 0)
        goto fail;
    IC_DATA_CMD = reg | (len == 0 ? DATA_CMD_STOP : 0);

    /* Write data bytes, STOP on the last */
    for (size_t i = 0; i < len; i++) {
        if (i2c_wait_tx_ready() < 0)
            goto fail;

        uint32_t cmd = buf[i];
        if (i == len - 1)
            cmd |= DATA_CMD_STOP;
        IC_DATA_CMD = cmd;
    }

    /* Wait for transfer to complete */
    if (i2c_wait_idle() < 0)
        goto fail;

    return 0;

fail:
    i2c_clear_abort();
    return -1;
}
