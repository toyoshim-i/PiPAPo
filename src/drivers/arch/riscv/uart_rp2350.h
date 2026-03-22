/*
 * uart_rp2350.h — RP2350 RISC-V UART extensions
 *
 * Baud rate switching for the 12 MHz → PLL transition.
 */

#ifndef PPAP_DRIVERS_ARCH_RISCV_UART_RP2350_H
#define PPAP_DRIVERS_ARCH_RISCV_UART_RP2350_H

void uart_tx_drain(void);
void uart_reinit_pll(void);

#endif /* PPAP_DRIVERS_ARCH_RISCV_UART_RP2350_H */
