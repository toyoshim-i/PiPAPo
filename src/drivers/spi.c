/*
 * spi.c — SPI0 driver for RP2040 PL022 (ARM PrimeCell SSP)
 *
 * Drives the SPI0 controller to communicate with the SD card on the
 * PicoCalc board.  Pin assignments are defined in src/board/picocalc.h.
 *
 * The PL022 operates in SPI master mode, 8-bit frames, CPOL=0, CPHA=0
 * (SD SPI mode 0).  CS is driven manually via GPIO (not the SSP's SSPFSSOUT).
 *
 * Baud rate: F_SPI = F_peri / (CPSDVSR × (1 + SCR))
 * where CPSDVSR is even (2–254) and SCR is 0–255.
 */

#include "spi.h"
#include "../board/picocalc.h"
#include <stdint.h>

/* ── Register access ────────────────────────────────────────────────────── */

#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* ── RESETS ──────────────────────────────────────────────────────────────── */

#define RESETS_RESET_DONE  REG(0x4000C008u)
#define RESETS_RESET_SET   REG(0x4000E000u)
#define RESETS_RESET_CLR   REG(0x4000F000u)

#define RESET_SPI0         (1u << 16)
#define RESET_IO_BANK0     (1u << 5)
#define RESET_PADS_BANK0   (1u << 8)

/* ── SPI0 (PL022) — base 0x4003C000 ─────────────────────────────────────── */

#define SPI0_BASE   0x4003C000u

#define SPI0_CR0    REG(SPI0_BASE + 0x00u)  /* Control Register 0          */
#define SPI0_CR1    REG(SPI0_BASE + 0x04u)  /* Control Register 1          */
#define SPI0_DR     REG(SPI0_BASE + 0x08u)  /* Data Register               */
#define SPI0_SR     REG(SPI0_BASE + 0x0Cu)  /* Status Register             */
#define SPI0_CPSR   REG(SPI0_BASE + 0x10u)  /* Clock Prescale Register     */

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

/* Pad defaults for SPI: IE=1, OD=0, DRIVE=1 (4mA), slew normal */
#define PAD_SPI_OUT     (0x50u)   /* IE=1, DRIVE=4mA               */
#define PAD_SPI_IN      (0x58u)   /* IE=1, PUE=1, DRIVE=4mA        */
#define PAD_GPIO_OUT    (0x50u)   /* IE=1, DRIVE=4mA               */
#define PAD_GPIO_IN     (0x5Cu)   /* IE=1, PUE=1, PDE=0            */

/* ── SIO — GPIO manual output 0xD0000000 ─────────────────────────────────── */

#define SIO_BASE           0xD0000000u
#define SIO_GPIO_OUT_SET   REG(SIO_BASE + 0x014u)
#define SIO_GPIO_OUT_CLR   REG(SIO_BASE + 0x018u)
#define SIO_GPIO_OE_SET    REG(SIO_BASE + 0x024u)
#define SIO_GPIO_OE_CLR    REG(SIO_BASE + 0x028u)
#define SIO_GPIO_IN        REG(SIO_BASE + 0x004u)

/* ── Configuration constant ──────────────────────────────────────────────── */

/* F_peri = 133 MHz after clock_init_pll().  Used for baud rate computation. */
#define F_PERI  133000000u

/* ── Internal helpers ───────────────────────────────────────────────────── */

static void gpio_set_func(uint32_t gpio, uint32_t funcsel)
{
    GPIO_CTRL(gpio) = funcsel;
}

static void compute_prescaler(uint32_t baud_hz, uint32_t *cpsdvsr, uint32_t *scr)
{
    /* F_SPI = F_peri / (CPSDVSR × (1 + SCR))
     * We want the highest frequency <= baud_hz.
     * Try CPSDVSR = 2,4,6,...,254; find smallest SCR that satisfies. */
    for (uint32_t cp = 2; cp <= 254; cp += 2) {
        uint32_t s = (F_PERI / (cp * baud_hz));
        if (s == 0) s = 0;
        else s -= 1;          /* SCR = ceil(F_peri/(cp*baud)) - 1 */
        /* Verify: F_peri / (cp * (1+s)) <= baud_hz */
        if (F_PERI / (cp * (1 + s)) > baud_hz) {
            s++;  /* bump SCR to reduce frequency */
        }
        if (s <= 255) {
            *cpsdvsr = cp;
            *scr = s;
            return;
        }
    }
    /* Slowest possible: CPSDVSR=254, SCR=255 */
    *cpsdvsr = 254;
    *scr = 255;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void spi_init(uint32_t baud_hz)
{
    /* 1. Release SPI0 from reset */
    RESETS_RESET_CLR = RESET_SPI0;
    while (!(RESETS_RESET_DONE & RESET_SPI0))
        ;

    /* 2. Configure GPIO pads */
    PAD_GPIO(PICOCALC_SPI0_SCK) = PAD_SPI_OUT;
    PAD_GPIO(PICOCALC_SPI0_TX)  = PAD_SPI_OUT;
    PAD_GPIO(PICOCALC_SPI0_RX)  = PAD_SPI_IN;
    PAD_GPIO(PICOCALC_SPI0_CS)  = PAD_GPIO_OUT;
    PAD_GPIO(PICOCALC_SD_CD)    = PAD_GPIO_IN;

    /* 3. Set GPIO functions: SCK/TX/RX → SPI0 (FUNCSEL 1), CS/CD → SIO */
    gpio_set_func(PICOCALC_SPI0_SCK, GPIO_FUNC_SPI);
    gpio_set_func(PICOCALC_SPI0_TX,  GPIO_FUNC_SPI);
    gpio_set_func(PICOCALC_SPI0_RX,  GPIO_FUNC_SPI);
    gpio_set_func(PICOCALC_SPI0_CS,  GPIO_FUNC_SIO);
    gpio_set_func(PICOCALC_SD_CD,    GPIO_FUNC_SIO);

    /* 4. CS pin: output, start de-asserted (high) */
    SIO_GPIO_OUT_SET = (1u << PICOCALC_SPI0_CS);
    SIO_GPIO_OE_SET  = (1u << PICOCALC_SPI0_CS);

    /* CD pin: input (OE disabled) */
    SIO_GPIO_OE_CLR = (1u << PICOCALC_SD_CD);

    /* 5. Disable SSP while configuring */
    SPI0_CR1 = 0;

    /* 6. Set baud rate */
    uint32_t cpsdvsr, scr;
    compute_prescaler(baud_hz, &cpsdvsr, &scr);
    SPI0_CPSR = cpsdvsr;

    /* 7. Configure: 8-bit, SPI mode 0, master, with SCR */
    SPI0_CR0 = CR0_DSS_8BIT | CR0_FRF_SPI | CR0_CPOL_0 | CR0_CPHA_0
             | (scr << 8);

    /* 8. Enable SSP (master mode: MS bit stays 0) */
    SPI0_CR1 = CR1_SSE;
}

void spi_set_baud(uint32_t baud_hz)
{
    /* Disable, reconfigure, re-enable */
    SPI0_CR1 &= ~CR1_SSE;

    uint32_t cpsdvsr, scr;
    compute_prescaler(baud_hz, &cpsdvsr, &scr);
    SPI0_CPSR = cpsdvsr;
    SPI0_CR0 = (SPI0_CR0 & 0x00FFu) | (scr << 8);

    SPI0_CR1 |= CR1_SSE;
}

uint8_t spi_xfer(uint8_t tx)
{
    /* Wait for TX FIFO space */
    while (!(SPI0_SR & SR_TNF))
        ;
    SPI0_DR = tx;

    /* Wait for RX data */
    while (!(SPI0_SR & SR_RNE))
        ;
    return (uint8_t)SPI0_DR;
}

void spi_xfer_block(const uint8_t *tx_buf, uint8_t *rx_buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t tx = tx_buf ? tx_buf[i] : 0xFFu;
        uint8_t rx = spi_xfer(tx);
        if (rx_buf)
            rx_buf[i] = rx;
    }
}

void sd_cs_low(void)
{
    SIO_GPIO_OUT_CLR = (1u << PICOCALC_SPI0_CS);
}

void sd_cs_high(void)
{
    SIO_GPIO_OUT_SET = (1u << PICOCALC_SPI0_CS);
    /* One extra byte to let SD card release MISO */
    (void)spi_xfer(0xFFu);
}

int spi_card_detect(void)
{
    /* GP22 is active-low: 0 = card present, 1 = no card */
    return (SIO_GPIO_IN & (1u << PICOCALC_SD_CD)) ? 0 : 1;
}
