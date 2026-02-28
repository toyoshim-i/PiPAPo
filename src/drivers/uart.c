/*
 * uart.c — Bare-metal UART0 driver for RP2040
 *
 * Initializes UART0 (PL011) at 115200 8N1 on GPIO 0 (TX) / GPIO 1 (RX).
 * All register access is direct (volatile pointers); no Pico SDK runtime
 * code is used.
 *
 * Before the baud rate divisor can be exact, the system clock must be on a
 * known frequency.  This file includes a minimal clock switch:
 *   ROSC (~6.5 MHz, imprecise) → XOSC (12 MHz crystal, stable)
 * Step 7 will extend this to a full clock.c that drives PLL_SYS to 133 MHz
 * and reconfigures UART with the updated divisor.
 */

#include "uart.h"
#include <stdint.h>

/* ==========================================================================
 * Register access helper
 * ========================================================================== */

#define REG(addr)  (*(volatile uint32_t *)(uintptr_t)(addr))

/* ==========================================================================
 * RESETS peripheral (base 0x4000C000)
 *
 * Atomic register aliases (RP2040 bus fabric feature):
 *   +0x0000  normal R/W
 *   +0x1000  XOR
 *   +0x2000  SET  (write 1 to set bits  → hold peripheral in reset)
 *   +0x3000  CLR  (write 1 to clear bits → release peripheral from reset)
 * ========================================================================== */

#define RESETS_RESET       REG(0x4000C000u)   /* reset control */
#define RESETS_RESET_DONE  REG(0x4000C008u)   /* 1 = peripheral out of reset */
#define RESETS_RESET_CLR   REG(0x4000F000u)   /* CLR alias: write 1 to unreset */

#define RESET_IO_BANK0     (1u << 5)          /* GPIO pin-function mux */
#define RESET_PADS_BANK0   (1u << 8)          /* GPIO pad settings */
#define RESET_UART0        (1u << 22)         /* UART0 (PL011) */

/* ==========================================================================
 * XOSC — Crystal oscillator (base 0x40024000)
 *
 * CTRL [11:0]  FREQ_RANGE : 0xAA0 = 1–15 MHz range (our 12 MHz crystal)
 * CTRL [23:12] ENABLE     : 0xFAB = enable, 0xD1E = disable
 * STATUS [31]  STABLE     : 1 when oscillator is locked and stable
 * ========================================================================== */

#define XOSC_CTRL    REG(0x40024000u)
#define XOSC_STATUS  REG(0x40024004u)

/* Combined CTRL value: FREQ_RANGE=1–15 MHz, ENABLE */
#define XOSC_CTRL_START   ((0xFABu << 12) | 0xAA0u)
#define XOSC_STATUS_STABLE (1u << 31)

/* ==========================================================================
 * CLOCKS peripheral (base 0x40008000)
 *
 * Each clock generator occupies 12 bytes: CTRL (+0), DIV (+4), SELECTED (+8)
 *   clk_ref  offset 0x30  — reference clock fed to clk_sys
 *   clk_sys  offset 0x3C  — system clock (CPU, bus fabric)
 *   clk_peri offset 0x48  — peripheral clock (UART, SPI, I2C, …)
 * ========================================================================== */

#define CLK_REF_CTRL      REG(0x40008030u)
#define CLK_REF_SELECTED  REG(0x40008038u)   /* one-hot: bit N = source N active */
#define CLK_SYS_CTRL      REG(0x4000803Cu)
#define CLK_SYS_SELECTED  REG(0x40008044u)
#define CLK_PERI_CTRL     REG(0x40008048u)

/* clk_ref SRC field [1:0]: 0=ROSC, 1=AUX, 2=XOSC */
#define CLK_REF_SRC_ROSC  0u
#define CLK_REF_SRC_XOSC  2u

/* clk_sys SRC bit [0]: 0=clk_ref (glitchless), 1=AUX */
#define CLK_SYS_SRC_REF   0u

/* clk_peri CTRL: AUXSRC[7:5]=0 (=clk_sys), ENABLE bit 11 */
#define CLK_PERI_ENABLE   (1u << 11)

/* ==========================================================================
 * IO_BANK0 — GPIO pin function select (base 0x40014000)
 *
 * Each GPIO: STATUS (offset 0, read-only), CTRL (offset +4) — 8-byte stride
 * CTRL [4:0] FUNCSEL: 2 = UART function
 * ========================================================================== */

#define GPIO0_CTRL  REG(0x40014004u)   /* GPIO 0 → UART0 TX */
#define GPIO1_CTRL  REG(0x4001400Cu)   /* GPIO 1 → UART0 RX */
#define GPIO_FUNC_UART  2u

/* ==========================================================================
 * UART0 — PL011 compatible (base 0x40034000)
 * ========================================================================== */

#define UART0_DR     REG(0x40034000u)  /* Data Register [7:0] */
#define UART0_FR     REG(0x40034018u)  /* Flag Register */
#define UART0_IBRD   REG(0x40034024u)  /* Integer Baud Rate Divisor */
#define UART0_FBRD   REG(0x40034028u)  /* Fractional Baud Rate Divisor */
#define UART0_LCR_H  REG(0x4003402Cu)  /* Line Control Register */
#define UART0_CR     REG(0x40034030u)  /* Control Register */

#define UART_FR_TXFF     (1u << 5)     /* FR: TX FIFO full — must wait */
#define UART_FR_BUSY     (1u << 3)     /* FR: UART busy (TX in progress or FIFO not empty) */

/* LCR_H: WLEN[6:5]=3 (8-bit), FEN[4]=1 (FIFOs on) → 0x70 */
#define UART_LCR_8N1_FIFO  ((3u << 5) | (1u << 4))

/* CR: UARTEN[0]=1, TXE[8]=1, RXE[9]=1 → 0x301 */
#define UART_CR_ENABLE  ((1u << 0) | (1u << 8) | (1u << 9))

/*
 * Baud rate divisors:
 *
 *   BAUDDIV = UARTCLK / (16 × baud)
 *
 *   @ 12 MHz XOSC:  12_000_000 / 1_843_200 = 6.5104  → IBRD=6,  FBRD=33
 *   @ 133 MHz PLL: 133_000_000 / 1_843_200 = 72.1701 → IBRD=72, FBRD=11
 */
#define UART_IBRD_115200_12MHZ    6u
#define UART_FBRD_115200_12MHZ   33u
#define UART_IBRD_115200_133MHZ  72u
#define UART_FBRD_115200_133MHZ  11u

/* ==========================================================================
 * Local helpers
 * ========================================================================== */

/*
 * unreset_periph — release one or more peripherals from reset and block
 * until the hardware confirms they are out of reset.
 */
static void unreset_periph(uint32_t mask)
{
    RESETS_RESET_CLR = mask;                    /* write to CLR alias */
    while ((RESETS_RESET_DONE & mask) != mask)  /* wait for all bits */
        ;
}

/*
 * clock_switch_to_xosc — switch system clock from ROSC to 12 MHz XOSC.
 *
 * After reset the RP2040 runs from the Ring Oscillator (~6.5 MHz, ±50%).
 * This function:
 *   1. Moves clk_sys → clk_ref  (glitchless, safe at any frequency)
 *   2. Moves clk_ref → ROSC     (ensure no AUX source is active)
 *   3. Starts the crystal oscillator and waits for it to stabilise
 *   4. Switches clk_ref → XOSC
 *   5. Enables clk_peri from clk_sys  (required by UART, SPI, I2C)
 *
 * After this function returns, the system clock is 12 MHz ±50 ppm.
 * Step 7 (clock.c) will call this first, then configure PLL_SYS to
 * 133 MHz and update the UART baud rate divisor.
 */
static void clock_switch_to_xosc(void)
{
    /* 1. Switch clk_sys to clk_ref (SRC bit 0 = 0).
     *    The glitchless mux ensures a clean, glitch-free transition. */
    CLK_SYS_CTRL = CLK_SYS_CTRL & ~1u;
    while (!(CLK_SYS_SELECTED & (1u << CLK_SYS_SRC_REF)))
        ;

    /* 2. Switch clk_ref to ROSC (SRC[1:0] = 0), clearing any AUX source. */
    CLK_REF_CTRL = CLK_REF_CTRL & ~3u;
    while (!(CLK_REF_SELECTED & (1u << CLK_REF_SRC_ROSC)))
        ;

    /* 3. Enable the crystal oscillator and wait for it to lock. */
    XOSC_CTRL = XOSC_CTRL_START;
    while (!(XOSC_STATUS & XOSC_STATUS_STABLE))
        ;

    /* 4. Switch clk_ref to XOSC (SRC = 2). */
    CLK_REF_CTRL = CLK_REF_SRC_XOSC;
    while (!(CLK_REF_SELECTED & (1u << CLK_REF_SRC_XOSC)))
        ;

    /* 5. Enable clk_peri sourced from clk_sys (AUXSRC=0=clk_sys, ENABLE=1).
     *    UART, SPI, and I2C all depend on clk_peri being active. */
    CLK_PERI_CTRL = CLK_PERI_ENABLE;
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

void uart_init_console(void)
{
    /* Switch to stable 12 MHz crystal before configuring baud rate */
    clock_switch_to_xosc();

    /* Release IO_BANK0 (GPIO mux), PADS_BANK0 (pad drive), UART0 from reset.
     * UART0 must be unreset AFTER clk_peri is running (done above). */
    unreset_periph(RESET_IO_BANK0 | RESET_PADS_BANK0 | RESET_UART0);

    /* Disable UART before changing any settings (PL011 requirement) */
    UART0_CR = 0;

    /* Set baud rate.  IBRD must be written before LCR_H; the baud rate
     * registers only latch into the internal divider when LCR_H is written. */
    UART0_IBRD = UART_IBRD_115200_12MHZ;
    UART0_FBRD = UART_FBRD_115200_12MHZ;

    /* 8N1 with FIFOs enabled.  Writing LCR_H commits the baud rate. */
    UART0_LCR_H = UART_LCR_8N1_FIFO;

    /* Enable UART, TX and RX */
    UART0_CR = UART_CR_ENABLE;

    /* Route GPIO 0 → UART0 TX, GPIO 1 → UART0 RX (function select = 2) */
    GPIO0_CTRL = GPIO_FUNC_UART;
    GPIO1_CTRL = GPIO_FUNC_UART;
}

void uart_putc(char c)
{
    while (UART0_FR & UART_FR_TXFF)  /* block while TX FIFO is full */
        ;
    UART0_DR = (uint32_t)(unsigned char)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');    /* expand LF → CR+LF for terminal compatibility */
        uart_putc(*s++);
    }
}

void uart_flush(void)
{
    while (UART0_FR & UART_FR_BUSY)  /* wait until TX FIFO empty and shift reg idle */
        ;
}

void uart_reinit_133mhz(void)
{
    /* Disable UART before touching the baud rate registers (PL011 requirement) */
    UART0_CR = 0;

    /* Update divisors for 115200 bps at 133 MHz clk_peri.
     * Writing LCR_H commits the new IBRD/FBRD values into the baud counter. */
    UART0_IBRD = UART_IBRD_115200_133MHZ;
    UART0_FBRD = UART_FBRD_115200_133MHZ;
    UART0_LCR_H = UART_LCR_8N1_FIFO;

    /* Re-enable UART, TX and RX */
    UART0_CR = UART_CR_ENABLE;
}
