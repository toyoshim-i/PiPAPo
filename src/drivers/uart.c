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
#include "config.h"
#include "../hw/rp2040.h"
#include "../hw/cortex_m0plus.h"
#include "kernel/fd/tty.h"      /* tty_rx_notify, tty_signal_intr */
#include <stdint.h>
#include <stddef.h>

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

#define UART_FR_TXFF     (1u << 5)     /* FR: TX FIFO full — must wait     */
#define UART_FR_RXFE     (1u << 4)     /* FR: RX FIFO empty                */
#define UART_FR_BUSY     (1u << 3)     /* FR: UART busy (shift reg active) */

/* LCR_H: WLEN[6:5]=3 (8-bit), FEN[4]=1 (FIFOs on) → 0x70 */
#define UART_LCR_8N1_FIFO  ((3u << 5) | (1u << 4))

/* CR: UARTEN[0]=1, TXE[8]=1, RXE[9]=1 → 0x301 */
#define UART_CR_ENABLE  ((1u << 0) | (1u << 8) | (1u << 9))

/* ==========================================================================
 * UART0 interrupt registers (base 0x40034000)
 * ========================================================================== */

#define UART0_IMSC   REG(0x40034038u)  /* Interrupt Mask Set/Clear          */
#define UART0_ICR    REG(0x40034044u)  /* Interrupt Clear (write-1-to-clear)*/

#define UART_IMSC_RXIM  (1u << 4)  /* mask: RX FIFO at/above level     */
#define UART_IMSC_TXIM  (1u << 5)  /* mask: TX FIFO at/below level     */
#define UART_IMSC_RTIM  (1u << 6)  /* mask: RX timeout (data in FIFO)  */

#define UART_ICR_RXIC   (1u << 4)  /* clear RX interrupt flag          */
#define UART_ICR_RTIC   (1u << 6)  /* clear RX timeout interrupt flag  */

/* NVIC_ISER is provided by cortex_m0plus.h */
#define UART0_IRQ_BIT   (1u << 20u)       /* UART0 = IRQ 20                */

/* ==========================================================================
 * TX/RX ring buffers — used only after uart_init_irq()
 *
 * TX: UART_TX_SIZE bytes (config.h), uint8_t indices auto-wrap at 256.
 *     One slot reserved to distinguish full from empty.
 *     Effective capacity: UART_TX_SIZE - 1 bytes.
 *     Empty: tx_head == tx_tail
 *     Full:  (uint8_t)(tx_head + 1) == tx_tail
 *
 * RX: UART_RX_SIZE bytes (config.h), uint8_t indices (0..255).
 *     Access via index & (UART_RX_SIZE-1).
 *     Capacity: UART_RX_SIZE bytes (count used for full check).
 *     Empty: rx_head == rx_tail
 *     Full:  (uint8_t)(rx_head - rx_tail) == UART_RX_SIZE
 * ========================================================================== */

static int              irq_mode;              /* 0 = polling, 1 = IRQ     */
static char             tx_buf[UART_TX_SIZE];
static char             rx_buf[UART_RX_SIZE];
static volatile uint8_t tx_head, tx_tail;
static volatile uint8_t rx_head, rx_tail;

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
    if (!irq_mode) {
        /* Polling mode: spin until TX FIFO has room */
        while (UART0_FR & UART_FR_TXFF)
            ;
        UART0_DR = (uint32_t)(unsigned char)c;
        return;
    }

    /* IRQ mode: write into TX ring buffer, arm TX interrupt.
     *
     * Critical section with interrupt-safe spin:
     *   We disable interrupts to protect the shared ring pointers, but
     *   re-enable them while waiting if the ring is full so the UART0 IRQ
     *   handler can drain it. */
    uint32_t primask;
    for (;;) {
        __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(primask));
        if ((uint8_t)(tx_head + 1u) != tx_tail)
            break;  /* ring has space; remain in critical section */
        /* Ring full — re-enable interrupts so UART0_IRQ_Handler can drain */
        __asm__ volatile("msr primask, %0" :: "r"(primask));
    }

    tx_buf[tx_head] = c;
    tx_head++;
    UART0_IMSC |= UART_IMSC_TXIM;   /* arm TX interrupt to drain ring */
    __asm__ volatile("msr primask, %0" :: "r"(primask));
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
    if (irq_mode) {
        /* IRQ mode: first wait for ring to drain into FIFO */
        while (tx_head != tx_tail)
            ;
    }
    while (UART0_FR & UART_FR_BUSY)  /* wait for FIFO + shift register to empty */
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

/* ==========================================================================
 * IRQ-mode API
 * ========================================================================== */

/*
 * UART0_IRQ_Handler — runs at interrupt priority, drains ring ↔ FIFO.
 *
 * TX path: while TX ring is non-empty and TX FIFO has room, move one byte.
 *          Disarm TXIM when the ring empties so the interrupt stops firing.
 *
 * RX path: while RX FIFO has data, move bytes into the RX ring.
 *          Drop silently on RX ring full (overrun; acceptable in Phase 1).
 *          Clear RXIC and RTIC so the interrupt de-asserts.
 */
void UART0_IRQ_Handler(void)
{
    /* TX: drain ring → TX FIFO */
    while (tx_head != tx_tail && !(UART0_FR & UART_FR_TXFF)) {
        UART0_DR = (uint32_t)(unsigned char)tx_buf[tx_tail];
        tx_tail++;
    }
    if (tx_head == tx_tail)
        UART0_IMSC &= ~UART_IMSC_TXIM;  /* ring empty — stop TX interrupts */

    /* RX: drain RX FIFO → ring */
    int got_rx = 0;
    int got_ctrl_c = 0;
    while (!(UART0_FR & UART_FR_RXFE)) {
        uint8_t c = (uint8_t)(UART0_DR & 0xFFu);
        if (c == 0x03u)
            got_ctrl_c = 1;
        if ((uint8_t)(rx_head - rx_tail) < UART_RX_SIZE)
            rx_buf[rx_head++ & (UART_RX_SIZE - 1u)] = (char)c;
        got_rx = 1;
    }
    /* Clear RX and RX-timeout interrupt flags */
    UART0_ICR = UART_ICR_RXIC | UART_ICR_RTIC;

    /* Wake processes blocked on tty read / deliver Ctrl-C signal */
    if (got_ctrl_c)
        tty_signal_intr();
    if (got_rx)
        tty_rx_notify();
}

void uart_init_irq(void)
{
    /* Leave UARTIFLS at its reset default (TXIFLSEL = ½, RXIFLSEL = ½).
     * Enable RX and RX-timeout interrupts.  TX interrupt is armed on demand
     * by uart_putc() so it only fires when there is data to send. */
    UART0_IMSC = UART_IMSC_RXIM | UART_IMSC_RTIM;

    /* Enable UART0 IRQ in the NVIC (bit 20 = IRQ 20) */
    NVIC_ISER = UART0_IRQ_BIT;

    irq_mode = 1;
}

int uart_getc(void)
{
    if (rx_head == rx_tail)
        return -1;  /* RX ring empty */
    int c = (unsigned char)rx_buf[rx_tail & (UART_RX_SIZE - 1u)];
    rx_tail++;
    return c;
}

int uart_rx_avail(void)
{
    return rx_head != rx_tail;
}

void uart_print_hex32(uint32_t v)
{
    uart_puts("0x");
    for (int i = 7; i >= 0; i--) {
        unsigned nibble = (v >> (i * 4)) & 0xFu;
        uart_putc(nibble < 10u ? (char)('0' + nibble) : (char)('a' + nibble - 10u));
    }
}

void uart_print_dec(uint32_t v)
{
    char buf[10];   /* 2^32 = 4294967296 — at most 10 digits */
    int  i = 0;
    if (v == 0u) { uart_putc('0'); return; }
    while (v > 0u) { buf[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (--i >= 0) uart_putc(buf[i]);
}
