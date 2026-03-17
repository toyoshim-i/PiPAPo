/*
 * uart_qemu.c — UART driver for QEMU mps2-an500 (CMSDK UART)
 *
 * Implements the same API as uart.c but targets the CMSDK UART0 at
 * 0x40004000 (ARM CoreLink SSE-050 Subsystem / mps2-an500 peripheral map).
 *
 * CMSDK UART register map (offsets from UART_BASE = 0x40004000):
 *   +0x00  DATA      [7:0] TX byte (write) / RX byte (read)
 *   +0x04  STATE     [0] TXFULL — wait if set before writing DATA
 *                    [1] RXFULL — data ready to read
 *   +0x08  CTRL      [0] TX_EN, [1] RX_EN, [2] TX_INT_EN, [3] RX_INT_EN
 *   +0x0C  INTSTATUS [0] TX interrupt, [1] RX interrupt (write-1-to-clear)
 *   +0x10  BAUDDIV   [19:0] baud rate = clk / BAUDDIV
 *
 * mps2-an500 interrupt map (see QEMU hw/arm/mps2.c uartirq[]):
 *   IRQ 0 = UART0 RX    IRQ 1 = UART0 TX
 *
 * mps2-an500 system clock: 25 MHz
 *   115200 bps → BAUDDIV = 25000000 / 115200 ≈ 217
 *
 * In QEMU, the CMSDK UART never actually blocks (TXFULL is always 0) and
 * data written to DATA appears on the QEMU serial output immediately.
 * The init steps are included for correctness on real mps2 hardware.
 *
 * No clock switch or baud rate reconfiguration is needed on QEMU.
 */

#include "drivers/uart.h"
#include "config.h"              /* UART_RX_SIZE */
#include "kernel/fd/tty.h"       /* tty_rx_notify, tty_signal_intr */
#include <stdint.h>

/* ==========================================================================
 * Register access helper
 * ========================================================================== */

#define REG(addr)  (*(volatile uint32_t *)(uintptr_t)(addr))

/* ==========================================================================
 * CMSDK UART0 — base 0x40004000
 * ========================================================================== */

#define UART_BASE    0x40004000u

#define UART_DATA    REG(UART_BASE + 0x00u)
#define UART_STATE   REG(UART_BASE + 0x04u)
#define UART_CTRL    REG(UART_BASE + 0x08u)
#define UART_BAUDDIV REG(UART_BASE + 0x10u)

#define UART_INTSTATUS REG(UART_BASE + 0x0Cu)

#define UART_STATE_TXFULL    (1u << 0)  /* TX buffer full — wait before writing */
#define UART_STATE_RXFULL    (1u << 1)  /* RX byte ready to read               */
#define UART_CTRL_TX_EN      (1u << 0)  /* enable transmitter  */
#define UART_CTRL_RX_EN      (1u << 1)  /* enable receiver     */
#define UART_CTRL_RX_INT_EN  (1u << 3)  /* enable RX interrupt */
#define UART_INT_RX          (1u << 1)  /* RX interrupt bit    */

/* NVIC: UART0 RX = IRQ 0 on mps2-an500 (QEMU hw/arm/mps2.c uartirq[]) */
#define NVIC_ISER    REG(0xE000E100u)
#define UART0RX_IRQ_BIT  (1u << 0)

/* 25 MHz system clock / 115200 bps */
#define UART_BAUDDIV_115200  217u

/* ==========================================================================
 * RX ring buffer
 * ========================================================================== */

static char             rx_buf[UART_RX_SIZE];
static volatile uint8_t rx_head, rx_tail;

/* ==========================================================================
 * Public API
 * ========================================================================== */

void uart_init(void)
{
    UART_BAUDDIV = UART_BAUDDIV_115200;
    UART_CTRL    = UART_CTRL_TX_EN | UART_CTRL_RX_EN | UART_CTRL_RX_INT_EN;

    /* Enable UART0 RX IRQ in NVIC (IRQ 0 on mps2-an500) */
    NVIC_ISER = UART0RX_IRQ_BIT;
}

int uart_putc(char c, void (*notify)(void))
{
    (void)notify;   /* polling — putc never fails, no callback needed */
    /* QEMU CMSDK UART: TXFULL is always 0 in the emulator — always succeeds */
    UART_DATA = (uint32_t)(unsigned char)c;
    return 1;
}

/* UART0 RX interrupt handler — CMSDK UART0 RX is IRQ 0 on mps2-an500.
 * Reads bytes into the ring buffer, clears the RX interrupt, and
 * notifies the tty layer to wake blocked readers. */
void UART0RX_IRQ_Handler(void)
{
    int got_rx = 0;
    while (UART_STATE & UART_STATE_RXFULL) {
        uint8_t c = (uint8_t)(UART_DATA & 0xFFu);
        /* Ctrl-C: deliver SIGINT immediately.  If ISIG is active,
         * tty_signal_intr() consumes the byte (don't queue it). */
        if (c == 0x03u && tty_signal_intr(TTY_SERIAL)) {
            got_rx = 1;
            continue;   /* consumed — skip ring buffer */
        }
        if ((uint8_t)(rx_head - rx_tail) < UART_RX_SIZE)
            rx_buf[rx_head++ & (UART_RX_SIZE - 1u)] = (char)c;
        got_rx = 1;
    }
    /* Clear RX interrupt */
    UART_INTSTATUS = UART_INT_RX;

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


