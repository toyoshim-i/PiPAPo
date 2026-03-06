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
#include "../hw/rp2040.h"
#include <stdint.h>

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

/*
 * PLL configuration for 133 MHz output:
 *   ref = XOSC / REFDIV = 12 MHz / 1 = 12 MHz
 *   VCO = ref × FBDIV   = 12 × 133 = 1596 MHz
 *   out = VCO / (POSTDIV1 × POSTDIV2) = 1596 / (6 × 2) = 133 MHz
 */
#define PLL_REFDIV           1u
#define PLL_FBDIV            133u
#define PLL_POSTDIV1         6u
#define PLL_POSTDIV2         2u
#define PLL_PRIM_VALUE       ((PLL_POSTDIV1 << 16) | (PLL_POSTDIV2 << 12))

/* CLK_SYS_CTRL AUXSRC field [7:5] */
#define CLK_SYS_AUXSRC_PLL  0u      /* AUXSRC = 0 → PLL_SYS */
#define CLK_SYS_SRC_AUX     1u      /* SRC = 1 → AUX mux    */

/* ==========================================================================
 * Public API
 * ========================================================================== */

/* ~500 ms timeout at 12 MHz (pre-PLL boot clock) */
#define PLL_TIMEOUT  6000000u

void clock_init_pll(void)
{
    uint32_t t;

    /* Step 1: Move clk_sys to clk_ref (SRC = 0) for a safe glitchless
     * transition — clk_sys must not be on the AUX mux while we reconfigure
     * PLL_SYS. */
    CLK_SYS_CTRL = CLK_SYS_CTRL & ~1u;
    t = PLL_TIMEOUT;
    while (!(CLK_SYS_SELECTED & 1u) && --t)   /* wait for clk_ref active */
        ;

    /* Step 2: Reset PLL_SYS, then release it so registers are at defaults. */
    RESETS_RESET_SET = RESET_PLL_SYS;
    RESETS_RESET_CLR = RESET_PLL_SYS;
    t = PLL_TIMEOUT;
    while (!(RESETS_RESET_DONE & RESET_PLL_SYS) && --t)
        ;

    /* Step 3: Program reference divisor and feedback divisor.
     * Must be written before powering up the VCO. */
    PLL_SYS_CS = PLL_REFDIV;          /* REFDIV = 1 → reference = 12 MHz */
    PLL_SYS_FBDIV_INT = PLL_FBDIV;    /* VCO = 12 × 133 = 1596 MHz       */

    /* Step 4: Power up the VCO and the main PLL (clear PD and VCOPD).
     * DSMPD and POSTDIVPD remain set until the VCO has locked. */
    PLL_SYS_PWR &= ~(PLL_PWR_PD | PLL_PWR_VCOPD);

    /* Step 5: Wait for the VCO to lock. */
    t = PLL_TIMEOUT;
    while (!(PLL_SYS_CS & PLL_CS_LOCK) && --t)
        ;

    /* Step 6: Program the post-dividers: POSTDIV1=6, POSTDIV2=2 → 133 MHz. */
    PLL_SYS_PRIM = PLL_PRIM_VALUE;

    /* Step 7: Power up the post-dividers. */
    PLL_SYS_PWR &= ~PLL_PWR_POSTDIVPD;

    /* Step 8: Point clk_sys AUX mux at PLL_SYS (AUXSRC = 0) and switch
     * clk_sys from clk_ref to the AUX mux (SRC = 1). */
    CLK_SYS_CTRL = (CLK_SYS_AUXSRC_PLL << 5) | CLK_SYS_SRC_AUX;
    t = PLL_TIMEOUT;
    while (!(CLK_SYS_SELECTED & (1u << CLK_SYS_SRC_AUX)) && --t)
        ;
}
