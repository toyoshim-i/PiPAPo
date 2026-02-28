/*
 * clock.c — PLL_SYS configuration for RP2040
 *
 * Switches the system clock from 12 MHz XOSC to 133 MHz via PLL_SYS.
 * Prerequisites: XOSC must be running and clk_ref must already be on XOSC
 * (uart_init_console() handles this).
 *
 * PLL_SYS target: 133 MHz
 *   Reference:   XOSC = 12 MHz, REFDIV = 1 → ref = 12 MHz
 *   VCO:         FBDIV_INT = 133 → VCO = 12 × 133 = 1596 MHz
 *   Post-divide: POSTDIV1 = 6, POSTDIV2 = 2 → output = 1596 / 12 = 133 MHz
 *
 * VCO frequency (1596 MHz) is within the RP2040's allowed range (400–1600 MHz).
 */

#include "clock.h"
#include <stdint.h>

/* ==========================================================================
 * Register access helper
 * ========================================================================== */

#define REG(addr)  (*(volatile uint32_t *)(uintptr_t)(addr))

/* ==========================================================================
 * RESETS peripheral — same atomic aliases as uart.c
 * ========================================================================== */

#define RESETS_RESET_DONE  REG(0x4000C008u)
#define RESETS_RESET_SET   REG(0x4000E000u)   /* SET alias: write 1 → assert reset  */
#define RESETS_RESET_CLR   REG(0x4000F000u)   /* CLR alias: write 1 → release reset */

#define RESET_PLL_SYS      (1u << 12)

/* ==========================================================================
 * PLL_SYS — base 0x40028000
 *
 * CS       +0x00  [5:0]  REFDIV, [31] LOCK (read-only)
 * PWR      +0x04  [0]    PD, [2] DSMPD, [3] POSTDIVPD, [5] VCOPD
 * FBDIV_INT+0x08  [11:0] integer feedback divisor
 * PRIM     +0x0C  [18:16] POSTDIV1, [14:12] POSTDIV2
 * ========================================================================== */

#define PLL_SYS_CS         REG(0x40028000u)
#define PLL_SYS_PWR        REG(0x40028004u)
#define PLL_SYS_FBDIV_INT  REG(0x40028008u)
#define PLL_SYS_PRIM       REG(0x4002800Cu)

#define PLL_CS_LOCK        (1u << 31)

#define PLL_PWR_PD         (1u << 0)
#define PLL_PWR_DSMPD      (1u << 2)
#define PLL_PWR_POSTDIVPD  (1u << 3)
#define PLL_PWR_VCOPD      (1u << 5)

/* ==========================================================================
 * CLOCKS peripheral — clk_sys generator (offset 0x3C)
 *
 * CLK_SYS_CTRL  0x4000803C
 *   SRC    [0]     0 = clk_ref (glitchless), 1 = AUX mux
 *   AUXSRC [7:5]   0 = PLL_SYS, 1 = PLL_USB, 2 = ROSC, 3 = XOSC, …
 *
 * CLK_SYS_SELECTED  0x40008044
 *   One-hot: bit 0 = clk_ref active, bit 1 = AUX mux active
 * ========================================================================== */

#define CLK_SYS_CTRL      REG(0x4000803Cu)
#define CLK_SYS_SELECTED  REG(0x40008044u)

/* ==========================================================================
 * Public API
 * ========================================================================== */

void clock_init_pll(void)
{
    /* Step 1: Move clk_sys to clk_ref (SRC = 0) for a safe glitchless
     * transition — clk_sys must not be on the AUX mux while we reconfigure
     * PLL_SYS. */
    CLK_SYS_CTRL = CLK_SYS_CTRL & ~1u;
    while (!(CLK_SYS_SELECTED & 1u))   /* wait for clk_ref active */
        ;

    /* Step 2: Reset PLL_SYS, then release it so registers are at defaults. */
    RESETS_RESET_SET = RESET_PLL_SYS;
    RESETS_RESET_CLR = RESET_PLL_SYS;
    while (!(RESETS_RESET_DONE & RESET_PLL_SYS))
        ;

    /* Step 3: Program reference divisor and feedback divisor.
     * Must be written before powering up the VCO. */
    PLL_SYS_CS = 1u;          /* REFDIV = 1 → reference = 12 MHz */
    PLL_SYS_FBDIV_INT = 133u; /* VCO = 12 × 133 = 1596 MHz       */

    /* Step 4: Power up the VCO and the main PLL (clear PD and VCOPD).
     * DSMPD and POSTDIVPD remain set until the VCO has locked. */
    PLL_SYS_PWR &= ~(PLL_PWR_PD | PLL_PWR_VCOPD);

    /* Step 5: Wait for the VCO to lock. */
    while (!(PLL_SYS_CS & PLL_CS_LOCK))
        ;

    /* Step 6: Program the post-dividers: POSTDIV1=6, POSTDIV2=2 → 133 MHz. */
    PLL_SYS_PRIM = (6u << 16) | (2u << 12);

    /* Step 7: Power up the post-dividers. */
    PLL_SYS_PWR &= ~PLL_PWR_POSTDIVPD;

    /* Step 8: Point clk_sys AUX mux at PLL_SYS (AUXSRC = 0) and switch
     * clk_sys from clk_ref to the AUX mux (SRC = 1). */
    CLK_SYS_CTRL = (0u << 5) | 1u;   /* AUXSRC=PLL_SYS, SRC=AUX */
    while (!(CLK_SYS_SELECTED & 2u)) /* wait for AUX mux active  */
        ;
}
