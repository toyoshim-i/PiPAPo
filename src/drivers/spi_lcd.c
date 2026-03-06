/*
 * spi_lcd.c — SPI1 LCD driver for PicoCalc 320×320 IPS display
 *
 * Drives the SPI1 (PL022) controller at ~33 MHz to send commands and pixel
 * data to the LCD.  Three additional GPIO pins provide chip-select (CS),
 * data/command (DC), and hardware reset (RST).
 *
 * This is a TX-only transport layer — the LCD has no MISO line.  The PL022
 * still fills its RX FIFO on every transfer, so we must drain it to prevent
 * overflow.
 *
 * Pin assignments come from src/target/pico1calc/pico1calc.h.
 */

#include "spi_lcd.h"
#include "../target/pico1calc/pico1calc.h"
#include "../hw/rp2040.h"
#include "config.h"
#include <stdint.h>

/* ── SPI1 (PL022) — base 0x4003D000 ─────────────────────────────────────── */

#define SPI1_BASE   0x4003D000u

#define SPI1_CR0    REG(SPI1_BASE + 0x00u)  /* Control Register 0          */
#define SPI1_CR1    REG(SPI1_BASE + 0x04u)  /* Control Register 1          */
#define SPI1_DR     REG(SPI1_BASE + 0x08u)  /* Data Register               */
#define SPI1_SR     REG(SPI1_BASE + 0x0Cu)  /* Status Register             */
#define SPI1_CPSR   REG(SPI1_BASE + 0x10u)  /* Clock Prescale Register     */

/* SSPCR0 fields */
#define CR0_DSS_8BIT   (0x07u)        /* 8-bit data size [3:0]       */
#define CR0_FRF_SPI    (0x00u << 4)   /* frame format: SPI [5:4]     */
#define CR0_CPOL_0     (0u << 6)      /* CPOL = 0                    */
#define CR0_CPHA_0     (0u << 7)      /* CPHA = 0                    */
/* SCR (serial clock rate) occupies bits [15:8] */

/* SSPCR1 fields */
#define CR1_SSE        (1u << 1)      /* SSP enable                  */

/* SSPSR fields */
#define SR_TFE         (1u << 0)      /* TX FIFO empty               */
#define SR_TNF         (1u << 1)      /* TX FIFO not full            */
#define SR_RNE         (1u << 2)      /* RX FIFO not empty           */
#define SR_BSY         (1u << 4)      /* SSP busy                    */

/* ── GPIO — IO_BANK0 0x40014000 ──────────────────────────────────────────── */

#define IO_BANK0_BASE   0x40014000u
#define GPIO_CTRL(n)    REG(IO_BANK0_BASE + (n) * 8u + 4u)

/* ── GPIO — PADS_BANK0 0x4001C000 ────────────────────────────────────────── */

#define PADS_BANK0_BASE 0x4001C000u
#define PAD_GPIO(n)     REG(PADS_BANK0_BASE + 4u + (n) * 4u)

#define PAD_SPI_OUT     (0x50u)   /* IE=1, DRIVE=4mA               */
#define PAD_GPIO_OUT    (0x50u)   /* IE=1, DRIVE=4mA               */

/* ── SIO — GPIO manual output 0xD0000000 ─────────────────────────────────── */

#define SIO_BASE           0xD0000000u
#define SIO_GPIO_OUT_SET   REG(SIO_BASE + 0x014u)
#define SIO_GPIO_OUT_CLR   REG(SIO_BASE + 0x018u)
#define SIO_GPIO_OE_SET    REG(SIO_BASE + 0x024u)

/* ── Pin masks ───────────────────────────────────────────────────────────── */

#define CS_MASK   (1u << PICOCALC_LCD_CS)
#define DC_MASK   (1u << PICOCALC_LCD_DC)
#define RST_MASK  (1u << PICOCALC_LCD_RST)

/* ── Internal helpers ───────────────────────────────────────────────────── */

static inline void cs_low(void)  { SIO_GPIO_OUT_CLR = CS_MASK; }
static inline void cs_high(void) { SIO_GPIO_OUT_SET = CS_MASK; }
static inline void dc_low(void)  { SIO_GPIO_OUT_CLR = DC_MASK; }
static inline void dc_high(void) { SIO_GPIO_OUT_SET = DC_MASK; }

static inline void drain_rx(void)
{
    while (SPI1_SR & SR_RNE)
        (void)SPI1_DR;
}

static inline void wait_idle(void)
{
    while (SPI1_SR & SR_BSY)
        ;
    drain_rx();
}

static void spi1_write_byte(uint8_t b)
{
    while (!(SPI1_SR & SR_TNF))
        ;
    SPI1_DR = b;
}

static void gpio_set_func(uint32_t gpio, uint32_t funcsel)
{
    GPIO_CTRL(gpio) = funcsel;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void spi_lcd_init(void)
{
    /* 1. Release SPI1 from reset */
    RESETS_RESET_CLR = RESET_SPI1;
    while (!(RESETS_RESET_DONE & RESET_SPI1))
        ;

    /* 2. Configure GPIO pads */
    PAD_GPIO(PICOCALC_SPI1_SCK) = PAD_SPI_OUT;
    PAD_GPIO(PICOCALC_SPI1_TX)  = PAD_SPI_OUT;
    PAD_GPIO(PICOCALC_LCD_CS)   = PAD_GPIO_OUT;
    PAD_GPIO(PICOCALC_LCD_DC)   = PAD_GPIO_OUT;
    PAD_GPIO(PICOCALC_LCD_RST)  = PAD_GPIO_OUT;

    /* 3. Set GPIO functions: SCK/TX → SPI1 (FUNCSEL 1), CS/DC/RST → SIO */
    gpio_set_func(PICOCALC_SPI1_SCK, GPIO_FUNC_SPI);
    gpio_set_func(PICOCALC_SPI1_TX,  GPIO_FUNC_SPI);
    gpio_set_func(PICOCALC_LCD_CS,   GPIO_FUNC_SIO);
    gpio_set_func(PICOCALC_LCD_DC,   GPIO_FUNC_SIO);
    gpio_set_func(PICOCALC_LCD_RST,  GPIO_FUNC_SIO);

    /* 4. CS/DC/RST: outputs, start de-asserted (high) */
    SIO_GPIO_OUT_SET = CS_MASK | DC_MASK | RST_MASK;
    SIO_GPIO_OE_SET  = CS_MASK | DC_MASK | RST_MASK;

    /* 5. Disable SSP while configuring */
    SPI1_CR1 = 0;

    /* 6. Set baud rate: ~33 MHz = 133 MHz / (CPSDVSR=2 × (1+SCR=1)) = 33.25 MHz */
    SPI1_CPSR = 2;

    /* 7. Configure: 8-bit, SPI mode 0, SCR=1 */
    SPI1_CR0 = CR0_DSS_8BIT | CR0_FRF_SPI | CR0_CPOL_0 | CR0_CPHA_0
             | (1u << 8);   /* SCR = 1 */

    /* 8. Enable SSP (master mode: MS bit stays 0) */
    SPI1_CR1 = CR1_SSE;
}

void spi_lcd_cmd(uint8_t cmd)
{
    wait_idle();
    dc_low();
    cs_low();
    spi1_write_byte(cmd);
    wait_idle();
    cs_high();
}

void spi_lcd_data(const uint8_t *buf, size_t len)
{
    dc_high();
    cs_low();
    for (size_t i = 0; i < len; i++) {
        spi1_write_byte(buf[i]);
        /* Drain RX periodically to prevent FIFO overflow (FIFO depth = 8) */
        if ((i & 7u) == 7u)
            drain_rx();
    }
    wait_idle();
    cs_high();
}

void spi_lcd_data16(const uint16_t *buf, size_t count)
{
    dc_high();
    cs_low();
    for (size_t i = 0; i < count; i++) {
        uint16_t v = buf[i];
        spi1_write_byte((uint8_t)(v >> 8));    /* MSB first */
        spi1_write_byte((uint8_t)(v & 0xFF));
        if ((i & 3u) == 3u)
            drain_rx();
    }
    wait_idle();
    cs_high();
}

void spi_lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t col[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)
    };
    uint8_t row[4] = {
        (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)
    };

    spi_lcd_cmd(0x2A);          /* Column Address Set */
    spi_lcd_data(col, 4);
    spi_lcd_cmd(0x2B);          /* Row Address Set */
    spi_lcd_data(row, 4);
    spi_lcd_cmd(0x2C);          /* Memory Write */
}

void spi_lcd_fill(uint16_t color, uint32_t count)
{
    uint8_t hi = (uint8_t)(color >> 8);
    uint8_t lo = (uint8_t)(color & 0xFF);

    dc_high();
    cs_low();
    for (uint32_t i = 0; i < count; i++) {
        spi1_write_byte(hi);
        spi1_write_byte(lo);
        if ((i & 3u) == 3u)
            drain_rx();
    }
    wait_idle();
    cs_high();
}
