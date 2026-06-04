/*
 * uart_rp2350.c — Polled UART0 driver for RP2350 RISC-V (Hazard3)
 *
 * PL011 UART0 on GPIO 0 (TX) / GPIO 1 (RX), 115200 8N1.
 * All I/O is polled — no ring buffer, no ISR, no NVIC.
 *
 * RISC-V on RP2350 has no NVIC; the Hazard3 external interrupt
 * controller (MEIE/MEIPA/MEIFA) is a different beast.  For the
 * initial port we keep things simple with blocking polled writes.
 */

#include <stddef.h>
#include <stdint.h>

#include "kernel/common/config.h"
#include "kernel/common/spinlock.h" /* SPIN_TXRING — SIO bus fence */
#include "kernel/vfs/driver/uart.h"
#include "kernel/vfs/tty.h" /* tty_rx_notify, tty_signal_intr */
#include "target/rpico.h"

/* ==========================================================================
 * XOSC — Crystal oscillator
 * ========================================================================== */

#define XOSC_CTRL REG(XOSC_BASE + 0x00u)
#define XOSC_STATUS REG(XOSC_BASE + 0x04u)
#define XOSC_STARTUP REG(XOSC_BASE + 0x0Cu)

#define XOSC_CTRL_FREQ_1_15MHZ 0xAA0u
#define XOSC_CTRL_ENABLE (0xFABu << 12)
#define XOSC_STATUS_STABLE (1u << 31)

/* Match the Pico SDK default: ceil(XOSC_KHz / 256) * 6.
 * For a 12 MHz crystal this is 47 * 6 = 282 cycles of 256*xtal_period. */
#define XOSC_STARTUP_DELAY_12MHZ 282u

/* ==========================================================================
 * PADS_BANK0 — GPIO pad configuration
 * ========================================================================== */

#define PAD_GPIO(n) REG(PADS_BANK0_BASE + 4u + (n) * 4u)
#define PAD_IE (1u << 6) /* input enable  */

/* ==========================================================================
 * IO_BANK0 — GPIO function select
 * ========================================================================== */

#define GPIO0_CTRL REG(IO_BANK0_BASE + 0x04u)
#define GPIO1_CTRL REG(IO_BANK0_BASE + 0x0Cu)
#define GPIO_FUNC_UART 2u

/* ==========================================================================
 * UART0 — PL011
 * ========================================================================== */

#define UART0_DR REG(UART0_BASE + 0x00u)
#define UART0_FR REG(UART0_BASE + 0x18u)
#define UART0_IBRD REG(UART0_BASE + 0x24u)
#define UART0_FBRD REG(UART0_BASE + 0x28u)
#define UART0_LCR_H REG(UART0_BASE + 0x2Cu)
#define UART0_CR REG(UART0_BASE + 0x30u)
#define UART0_IMSC REG(UART0_BASE + 0x38u)
#define UART0_ICR REG(UART0_BASE + 0x44u)

#define UART_FR_TXFF (1u << 5)
#define UART_FR_RXFE (1u << 4)
#define UART_FR_BUSY (1u << 3)

#define UART_LCR_8N1_FIFO ((3u << 5) | (1u << 4))
#define UART_CR_ENABLE ((1u << 0) | (1u << 8) | (1u << 9))

/* ==========================================================================
 * Baud rate divisors
 * ========================================================================== */

#define UART_BAUD 115200u

/* @ 12 MHz XOSC */
#define UART_IBRD_12MHZ 6u
#define UART_FBRD_12MHZ 33u

/* @ PPAP_SYS_HZ (PLL) */
#define UART_IBRD_PLL (PPAP_SYS_HZ / (16u * UART_BAUD))
#define UART_FBRD_PLL ((PPAP_SYS_HZ % (16u * UART_BAUD)) * 4u / UART_BAUD)

/* ==========================================================================
 * Local helpers
 * ========================================================================== */

static void unreset_periph(uint32_t mask) {
  RESETS_RESET_CLR = mask;
  while ((RESETS_RESET_DONE & mask) != mask);
}

static void clock_switch_to_xosc(void) {
  /* Move clk_sys to clk_ref (glitchless mux) */
  CLK_SYS_CTRL = CLK_SYS_CTRL & ~1u;
  while (!(CLK_SYS_SELECTED & (1u << CLK_SYS_SRC_REF)));

  /* Move clk_ref to ROSC (safe source while starting XOSC) */
  CLK_REF_CTRL = CLK_REF_CTRL & ~3u;
  while (!(CLK_REF_SELECTED & (1u << CLK_REF_SRC_ROSC)));

  /* Program the XOSC exactly once before enabling it. The startup delay is
   * important on RP2350: leaving it at reset/default can make crystal bring-up
   * timing-dependent across boots. */
  XOSC_CTRL = XOSC_CTRL_FREQ_1_15MHZ;
  XOSC_STARTUP = XOSC_STARTUP_DELAY_12MHZ;
  XOSC_CTRL = XOSC_CTRL_ENABLE | XOSC_CTRL_FREQ_1_15MHZ;
  while (!(XOSC_STATUS & XOSC_STATUS_STABLE));

  /* Switch clk_ref to XOSC */
  CLK_REF_CTRL = CLK_REF_SRC_XOSC;
  while (!(CLK_REF_SELECTED & (1u << CLK_REF_SRC_XOSC)));

  /* Enable clk_peri from clk_sys (AUXSRC=0), divider 1:1 */
  CLK_PERI_DIV = 0x10000u;
  CLK_PERI_CTRL = CLK_PERI_ENABLE;
}

/* ==========================================================================
 * RX ring buffer (interrupt-less; polled from uart_getc)
 * ========================================================================== */

#define RX_SIZE UART_RX_SIZE
static char rx_buf[RX_SIZE];
static volatile uint8_t rx_head, rx_tail;

/* Drain HW RX FIFO into ring — call periodically or before uart_getc. */
static void rx_poll(void) {
  while (!(UART0_FR & UART_FR_RXFE)) {
    uint8_t c = (uint8_t)(UART0_DR & 0xFFu);
    if (c == 0x03u && tty_signal_intr(TTY_SERIAL)) continue;
    if ((uint8_t)(rx_head - rx_tail) < RX_SIZE)
      rx_buf[rx_head++ & (RX_SIZE - 1u)] = (char)c;
  }
}

/* ==========================================================================
 * Public API
 * ========================================================================== */

void uart_init(void) {
  clock_switch_to_xosc();

  /* Hard-reset UART0 then bring it and GPIO out of reset */
  RESETS_RESET_SET = RESET_UART0;
  unreset_periph(RESET_IO_BANK0 | RESET_PADS_BANK0 | RESET_UART0);

  UART0_CR = 0;
  UART0_LCR_H = 0;    /* flush FIFOs */
  UART0_ICR = 0x7FFu; /* clear all interrupt flags */
  UART0_IMSC = 0;     /* no interrupts */

  /* Baud rate for 115200 @ 12 MHz */
  UART0_IBRD = UART_IBRD_12MHZ;
  UART0_FBRD = UART_FBRD_12MHZ;
  UART0_LCR_H = UART_LCR_8N1_FIFO;
  UART0_CR = UART_CR_ENABLE;

  /* Route GPIO 0 → TX, GPIO 1 → RX */
  GPIO0_CTRL = GPIO_FUNC_UART;
  GPIO1_CTRL = GPIO_FUNC_UART;

  /* Configure pads: clear ISO (RP2350 default=1), enable IE */
  PAD_GPIO(0) = PAD_IE | (2u << 4);
  PAD_GPIO(1) = PAD_IE | (2u << 4);
}

int uart_putc(char c, void (*notify)(void)) {
  (void)notify; /* no ISR callback in polled mode */

  /* The SIO spinlock acquire/release acts as a cross-bus-domain fence
   * on RP2350.  Without it, the Hazard3 core may read stale UART0_FR
   * values from the APB bus, causing FIFO overwrites and garbled output. */
  uint32_t saved = spin_lock_irqsave(SPIN_TXRING);
  while (UART0_FR & UART_FR_TXFF);
  UART0_DR = (uint32_t)(unsigned char)c;
  spin_unlock_irqrestore(SPIN_TXRING, saved);
  return 1;
}

int uart_getc(void) {
  rx_poll();
  if (rx_head == rx_tail) return -1;
  int c = (unsigned char)rx_buf[rx_tail & (RX_SIZE - 1u)];
  rx_tail++;
  return c;
}

int uart_rx_avail(void) {
  rx_poll();
  return rx_head != rx_tail;
}

/* ==========================================================================
 * Baud rate switching (12 MHz → PLL)
 * ========================================================================== */

void uart_tx_drain(void) {
  /* Polled driver — nothing in a ring to drain.
   * Just wait for the shift register to finish.  Keep this bounded because
   * RP2350 Hazard3 can report BUSY spuriously after some failed boots. */
  for (uint32_t timeout = 200000u; (UART0_FR & UART_FR_BUSY) && timeout;
       timeout--);
}

void uart_reinit_pll(void) {
  UART0_CR = 0;
  UART0_IBRD = UART_IBRD_PLL;
  UART0_FBRD = UART_FBRD_PLL;
  UART0_LCR_H = UART_LCR_8N1_FIFO; /* latch IBRD/FBRD */
  UART0_CR = UART_CR_ENABLE;
  UART0_ICR = 0x7FFu;
}
