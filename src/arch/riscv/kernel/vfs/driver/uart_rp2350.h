/*
 * uart_rp2350.h — RP2350 RISC-V UART extensions
 *
 * Baud rate switching for the 12 MHz → PLL transition.
 */

#ifndef PPAP_ARCH_RISCV_KERNEL_VFS_DRIVER_UART_RP2350_H
#define PPAP_ARCH_RISCV_KERNEL_VFS_DRIVER_UART_RP2350_H

void uart_tx_drain(void);
void uart_reinit_pll(void);

#endif /* PPAP_ARCH_RISCV_KERNEL_VFS_DRIVER_UART_RP2350_H */
