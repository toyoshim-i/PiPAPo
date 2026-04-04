/*
 * clock.h — System clock driver interface
 *
 * clock_init_pll() configures PLL_SYS from the 12 MHz XOSC to the target
 * frequency (PPAP_SYS_HZ).  PLL dividers are set per target via -D flags.
 * See drivers/arch/arm_m/uart_rpico.h for the UART baud rate switch
 * sequence around this call.
 */

#ifndef PPAP_DRIVERS_CLOCK_H
#define PPAP_DRIVERS_CLOCK_H

/* Configure PLL_SYS and switch clk_sys to it.
 * Output frequency is determined by PPAP_PLL_FBDIV / PPAP_PLL_PD1 / PD2.
 * Prerequisite: XOSC must already be running (uart_init() does this).
 * After this call clk_sys = clk_peri = PPAP_SYS_HZ. */
void clock_init_pll(void);

#endif /* PPAP_DRIVERS_CLOCK_H */
