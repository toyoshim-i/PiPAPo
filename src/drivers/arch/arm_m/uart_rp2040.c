/*
 * uart_rp2040.c — Bare-metal UART0 driver for RP2040
 *
 * Initializes UART0 (PL011) at 115200 8N1 on GPIO 0 (TX) / GPIO 1 (RX).
 * All register access is direct (volatile pointers); no Pico SDK runtime
 * code is used.
 *
 * TX architecture: a 255-byte ring buffer sits between all writers and the
 * UART HW FIFO.  uart_putc() enqueues one byte; if the ring is full it
 * drains ring → FIFO inline.  The ISR also drains the ring asynchronously.
 *
 * The ISR is enabled from uart_init() onward.  klog holds SPIN_UART for
 * atomic output; uart_putc holds SPIN_TXRING per character.
 */

#include "drivers/uart.h"
#include "config.h"
#include "target/rp2040.h"
#include "arch/arm_m/cpu.h"
#include "kernel/fd/tty.h"      /* tty_rx_notify, tty_signal_intr */
#include "kernel/spinlock.h"    /* SPIN_TXRING */
#include <stdint.h>
#include <stddef.h>

/* ==========================================================================
 * XOSC — Crystal oscillator (base 0x40024000)
 * ========================================================================== */

#define XOSC_CTRL    REG(0x40024000u)
#define XOSC_STATUS  REG(0x40024004u)

#define XOSC_CTRL_START   ((0xFABu << 12) | 0xAA0u)
#define XOSC_STATUS_STABLE (1u << 31)

/* ==========================================================================
 * IO_BANK0 — GPIO pin function select (base 0x40014000)
 * ========================================================================== */

#define GPIO0_CTRL  REG(0x40014004u)   /* GPIO 0 → UART0 TX */
#define GPIO1_CTRL  REG(0x4001400Cu)   /* GPIO 1 → UART0 RX */
#define GPIO_FUNC_UART  2u

/* ==========================================================================
 * UART0 — PL011 compatible (base 0x40034000)
 * ========================================================================== */

#define UART0_DR     REG(0x40034000u)
#define UART0_FR     REG(0x40034018u)
#define UART0_IBRD   REG(0x40034024u)
#define UART0_FBRD   REG(0x40034028u)
#define UART0_LCR_H  REG(0x4003402Cu)
#define UART0_CR     REG(0x40034030u)

#define UART_FR_TXFF     (1u << 5)
#define UART_FR_RXFE     (1u << 4)
#define UART_FR_BUSY     (1u << 3)

#define UART_LCR_8N1_FIFO  ((3u << 5) | (1u << 4))
#define UART_CR_ENABLE  ((1u << 0) | (1u << 8) | (1u << 9))

/* ==========================================================================
 * UART0 interrupt registers
 * ========================================================================== */

#define UART0_IMSC   REG(0x40034038u)
#define UART0_RIS    REG(0x4003403Cu)   /* Raw Interrupt Status              */
#define UART0_MIS    REG(0x40034040u)   /* Masked Interrupt Status           */
#define UART0_ICR    REG(0x40034044u)

#define UART_IMSC_RXIM  (1u << 4)
#define UART_IMSC_TXIM  (1u << 5)
#define UART_IMSC_RTIM  (1u << 6)

#define UART_ICR_RXIC   (1u << 4)
#define UART_ICR_RTIC   (1u << 6)

#define UART0_IRQ_BIT   (1u << 20u)

/* ==========================================================================
 * TX/RX ring buffers
 *
 * TX: UART_TX_SIZE bytes, uint8_t indices (auto-wrap at 256).
 *     Effective capacity: UART_TX_SIZE - 1 bytes.
 * RX: UART_RX_SIZE bytes, index masked to power-of-2.
 * ========================================================================== */

_Static_assert(UART_TX_SIZE == 256u,
               "UART_TX_SIZE must be 256 — uint8_t index wraparound assumes this");

static char             tx_buf[UART_TX_SIZE];
static char             rx_buf[UART_RX_SIZE];
static volatile uint8_t tx_head, tx_tail;
static volatile uint8_t rx_head, rx_tail;

/*
 * Baud rate divisors:
 *   @ 12 MHz XOSC:  IBRD=6,  FBRD=33
 *   @ 133 MHz PLL:  IBRD=72, FBRD=11
 */
#define UART_IBRD_115200_12MHZ    6u
#define UART_FBRD_115200_12MHZ   33u
#define UART_IBRD_115200_133MHZ  72u
#define UART_FBRD_115200_133MHZ  11u

/* ==========================================================================
 * Local helpers
 * ========================================================================== */

static void unreset_periph(uint32_t mask)
{
    RESETS_RESET_CLR = mask;
    while ((RESETS_RESET_DONE & mask) != mask)
        ;
}

static void clock_switch_to_xosc(void)
{
    CLK_SYS_CTRL = CLK_SYS_CTRL & ~1u;
    while (!(CLK_SYS_SELECTED & (1u << CLK_SYS_SRC_REF)))
        ;

    CLK_REF_CTRL = CLK_REF_CTRL & ~3u;
    while (!(CLK_REF_SELECTED & (1u << CLK_REF_SRC_ROSC)))
        ;

    XOSC_CTRL = XOSC_CTRL_START;
    while (!(XOSC_STATUS & XOSC_STATUS_STABLE))
        ;

    CLK_REF_CTRL = CLK_REF_SRC_XOSC;
    while (!(CLK_REF_SELECTED & (1u << CLK_REF_SRC_XOSC)))
        ;

    CLK_PERI_CTRL = CLK_PERI_ENABLE;
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

void uart_init(void)
{
    /* Switch to stable 12 MHz crystal before configuring baud rate */
    clock_switch_to_xosc();

    /* Disable UART0 IRQ in NVIC — the UF2 bootloader may leave it enabled */
    NVIC_ICER = UART0_IRQ_BIT;
    NVIC_ICPR = UART0_IRQ_BIT;

    /* Hard-reset UART0 to clear all internal state */
    RESETS_RESET_SET = RESET_UART0;
    unreset_periph(RESET_IO_BANK0 | RESET_PADS_BANK0 | RESET_UART0);

    UART0_CR = 0;
    UART0_LCR_H = 0;              /* flush FIFOs */
    UART0_ICR = 0x7FFu;           /* clear all interrupt flags */
    UART0_IMSC = 0;

    /* Set baud rate for 115200 @ 12 MHz */
    UART0_IBRD = UART_IBRD_115200_12MHZ;
    UART0_FBRD = UART_FBRD_115200_12MHZ;
    UART0_LCR_H = UART_LCR_8N1_FIFO;
    UART0_CR = UART_CR_ENABLE;

    /* Route GPIO 0 → TX, GPIO 1 → RX */
    GPIO0_CTRL = GPIO_FUNC_UART;
    GPIO1_CTRL = GPIO_FUNC_UART;

    /* Enable RX + RX-timeout interrupts.  TX interrupt (TXIM) is armed
     * on demand by uart_putc() when data enters the ring. */
    UART0_IMSC = UART_IMSC_RXIM | UART_IMSC_RTIM;
    NVIC_ICPR = UART0_IRQ_BIT;
    NVIC_ISER = UART0_IRQ_BIT;
}

/* TX-ready callback — registered atomically by uart_putc() on failure */
static void (*tx_notify_cb)(void);

int uart_putc(char c, void (*notify)(void))
{
    uint32_t saved = spin_lock_irqsave(SPIN_TXRING);

    /* If ring is full, try to free space by draining ring → HW FIFO. */
    if ((uint8_t)(tx_head + 1u) == tx_tail) {
        while (tx_tail != tx_head && !(UART0_FR & UART_FR_TXFF)) {
            UART0_DR = (uint32_t)(unsigned char)tx_buf[tx_tail];
            tx_tail++;
        }
        if ((uint8_t)(tx_head + 1u) == tx_tail) {
            /* Ring AND FIFO both full — arm ISR for when FIFO drains. */
            UART0_IMSC |= UART_IMSC_TXIM;
            if (notify)
                tx_notify_cb = notify;
            spin_unlock_irqrestore(SPIN_TXRING, saved);
            return 0;
        }
    }

    tx_buf[tx_head] = c;
    tx_head++;

    /* Kick: drain ring → HW FIFO while FIFO has space.
     *
     * The PL011 TX interrupt is TRANSITION-based: TXRIS only asserts
     * when the FIFO level drops from above the watermark to at-or-below
     * it.  Writing directly to the FIFO here ensures bytes flow out
     * immediately.  When the FIFO is full the ISR takes over once the
     * level crosses the watermark. */
    while (tx_tail != tx_head && !(UART0_FR & UART_FR_TXFF)) {
        UART0_DR = (uint32_t)(unsigned char)tx_buf[tx_tail];
        tx_tail++;
    }
    if (tx_tail != tx_head)
        UART0_IMSC |= UART_IMSC_TXIM;

    spin_unlock_irqrestore(SPIN_TXRING, saved);
    return 1;
}

void uart_tx_drain(void)
{
    /* Drain ring → HW FIFO, then wait for shift register idle.
     * Call before clock_init_pll() so all pending bytes are sent
     * at the current baud rate. */
    while (tx_head != tx_tail) {
        uint32_t saved = spin_lock_irqsave(SPIN_TXRING);
        while (tx_head != tx_tail && !(UART0_FR & UART_FR_TXFF)) {
            UART0_DR = (uint32_t)(unsigned char)tx_buf[tx_tail];
            tx_tail++;
        }
        spin_unlock_irqrestore(SPIN_TXRING, saved);
    }
    while (UART0_FR & UART_FR_BUSY)
        ;
    /* Ring is empty — clear TXIM so the next uart_putc burst will kick
     * bytes directly into the FIFO instead of waiting for an ISR that
     * has nothing to drain. */
    UART0_IMSC &= ~UART_IMSC_TXIM;
}

void uart_reinit_133mhz(void)
{
    /* Minimal baud rate switch for 115200 @ 133 MHz.
     * Prerequisite: uart_tx_drain() then clock_init_pll().
     *
     * Only the baud divisors are changed.  All other UART0 state (IMSC,
     * GPIO routing, NVIC) is preserved from uart_init(). */
    /* Disable UART before touching baud rate registers (PL011 requirement) */
    UART0_CR = 0;

    UART0_IBRD = UART_IBRD_115200_133MHZ;
    UART0_FBRD = UART_FBRD_115200_133MHZ;
    UART0_LCR_H = UART_LCR_8N1_FIFO;  /* latch IBRD/FBRD (PL011 req) */

    /* Re-enable UART, TX and RX */
    UART0_CR = UART_CR_ENABLE;

    UART0_ICR  = 0x7FFu;       /* clear stale interrupt flags           */
}

/* ==========================================================================
 * UART0 ISR
 * ========================================================================== */

void UART0_IRQ_Handler(void)
{
    /* TX: drain ring → TX FIFO, wake blocked writers */
    if (UART0_IMSC & UART_IMSC_TXIM) {
        uint32_t tx_saved = spin_lock_irqsave(SPIN_TXRING);
        while (tx_head != tx_tail && !(UART0_FR & UART_FR_TXFF)) {
            UART0_DR = (uint32_t)(unsigned char)tx_buf[tx_tail];
            tx_tail++;
        }
        if (tx_head == tx_tail)
            UART0_IMSC &= ~UART_IMSC_TXIM;
        spin_unlock_irqrestore(SPIN_TXRING, tx_saved);
        /* Ring has space — notify TTY layer to wake blocked writers */
        if (tx_notify_cb)
            tx_notify_cb();
    }

    /* RX: drain RX FIFO → ring */
    int got_rx = 0;
    while (!(UART0_FR & UART_FR_RXFE)) {
        uint8_t c = (uint8_t)(UART0_DR & 0xFFu);
        if (c == 0x03u && tty_signal_intr(TTY_SERIAL)) {
            got_rx = 1;
            continue;
        }
        if ((uint8_t)(rx_head - rx_tail) < UART_RX_SIZE)
            rx_buf[rx_head++ & (UART_RX_SIZE - 1u)] = (char)c;
        got_rx = 1;
    }
    UART0_ICR = UART_ICR_RXIC | UART_ICR_RTIC;

    if (got_rx)
        tty_rx_notify(TTY_SERIAL);
}

int uart_getc(void)
{
    if (rx_head == rx_tail)
        return -1;
    int c = (unsigned char)rx_buf[rx_tail & (UART_RX_SIZE - 1u)];
    rx_tail++;
    return c;
}

int uart_rx_avail(void)
{
    return rx_head != rx_tail;
}

