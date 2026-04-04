/*
 * clock.c — PLL_SYS configuration for RP2040/RP2350
 *
 * Switches the system clock from 12 MHz XOSC to the target frequency via
 * PLL_SYS.  PLL divider values are overridable per target via -D flags:
 *   PPAP_PLL_FBDIV, PPAP_PLL_PD1, PPAP_PLL_PD2
 *
 * Defaults (RP2040): FBDIV=133, PD1=6, PD2=2 → 133 MHz
 * RP2350 example:    FBDIV=125, PD1=5, PD2=2 → 150 MHz
 *
 * Prerequisites: XOSC must be running and clk_ref must already be on XOSC
 * (uart_init() handles this).
 */

#include <stdint.h>

#include "kernel/core/driver/clock.h"
#include "target/rpico.h"

/* ==========================================================================
 * PLL_SYS
 *
 * CS       +0x00  [5:0]  REFDIV, [31] LOCK (read-only)
 * PWR      +0x04  [0]    PD, [2] DSMPD, [3] POSTDIVPD, [5] VCOPD
 * FBDIV_INT+0x08  [11:0] integer feedback divisor
 * PRIM     +0x0C  [18:16] POSTDIV1, [14:12] POSTDIV2
 * ========================================================================== */

#define PLL_SYS_CS REG(PLL_SYS_BASE + 0x00u)
#define PLL_SYS_PWR REG(PLL_SYS_BASE + 0x04u)
#define PLL_SYS_FBDIV_INT REG(PLL_SYS_BASE + 0x08u)
#define PLL_SYS_PRIM REG(PLL_SYS_BASE + 0x0Cu)

#define PLL_CS_LOCK (1u << 31)

#define PLL_PWR_PD (1u << 0)
#define PLL_PWR_DSMPD (1u << 2)
#define PLL_PWR_POSTDIVPD (1u << 3)
#define PLL_PWR_VCOPD (1u << 5)

/*
 * PLL configuration — overridable per target via -D flags in CMakeLists.txt.
 *
 * Defaults (RP2040, 133 MHz):
 *   ref = XOSC / REFDIV = 12 MHz / 1 = 12 MHz
 *   VCO = ref × FBDIV   = 12 × 133 = 1596 MHz
 *   out = VCO / (POSTDIV1 × POSTDIV2) = 1596 / (6 × 2) = 133 MHz
 *
 * RP2350 override example (150 MHz):
 *   -DPPAP_PLL_FBDIV=125 -DPPAP_PLL_PD1=5 -DPPAP_PLL_PD2=2
 *   VCO = 12 × 125 = 1500 MHz, out = 1500 / (5 × 2) = 150 MHz
 */
#define PLL_REFDIV 1u

#ifndef PPAP_PLL_FBDIV
#define PPAP_PLL_FBDIV 133u
#endif
#ifndef PPAP_PLL_PD1
#define PPAP_PLL_PD1 6u
#endif
#ifndef PPAP_PLL_PD2
#define PPAP_PLL_PD2 2u
#endif

#define PLL_FBDIV PPAP_PLL_FBDIV
#define PLL_POSTDIV1 PPAP_PLL_PD1
#define PLL_POSTDIV2 PPAP_PLL_PD2
#define PLL_PRIM_VALUE ((PLL_POSTDIV1 << 16) | (PLL_POSTDIV2 << 12))

/* CLK_SYS_CTRL AUXSRC field [7:5] */
#define CLK_SYS_AUXSRC_PLL 0u /* AUXSRC = 0 → PLL_SYS */
#define CLK_SYS_SRC_AUX 1u    /* SRC = 1 → AUX mux    */

/* ==========================================================================
 * Public API
 * ========================================================================== */

/* ~500 ms timeout at 12 MHz (pre-PLL boot clock) */
#define PLL_TIMEOUT 6000000u

/*
 * clock_init_pll must run from SRAM on RP2350 RISC-V.
 *
 * On RP2350, bootrom_state_reset(GLOBAL_STATE) resets default resource
 * permissions for all bus masters.  This can invalidate the XIP cache's
 * ability to refill from flash.  Initially-cached instructions continue
 * to execute, but once we reach a cache-line boundary deep in this
 * function (the PLL VCO lock polling loop), the refill fails with an
 * instruction access fault (mcause=1).
 *
 * Placing this function in .ramfunc eliminates all flash dependency
 * during the clock transition.  boot.S copies .ramfunc from flash to
 * SRAM before calling kmain().
 */
__attribute__((section(".ramfunc.clock_init_pll")))
void clock_init_pll(void) {
  uint32_t t;

  /* The SDK clears resus before clock reconfiguration so a previous session's
   * clock fault recovery doesn't interfere with our manual tree switch. */
  CLK_SYS_RESUS_CTRL = 0;

  /* Step 1: Move clk_sys to clk_ref (SRC = 0) for a safe glitchless
   * transition — clk_sys must not be on the AUX mux while we reconfigure
   * PLL_SYS. */
  CLK_SYS_CTRL = CLK_SYS_CTRL & ~1u;
  t = PLL_TIMEOUT;
  while (!(CLK_SYS_SELECTED & 1u) && --t) /* wait for clk_ref active */
    ;

  /* Step 2: Reset PLL_SYS, then release it so registers are at defaults. */
  RESETS_RESET_SET = RESET_PLL_SYS;
  RESETS_RESET_CLR = RESET_PLL_SYS;
  t = PLL_TIMEOUT;
  while (!(RESETS_RESET_DONE & RESET_PLL_SYS) && --t)
    ;

  /* Step 3: Program reference divisor and feedback divisor.
   * Must be written before powering up the VCO. */
  PLL_SYS_CS = PLL_REFDIV;       /* REFDIV = 1 → reference = 12 MHz */
  PLL_SYS_FBDIV_INT = PLL_FBDIV; /* VCO = 12 × 133 = 1596 MHz       */

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

  /* Step 9: Reconfigure clk_peri for the new clock speed.
   * clk_peri has no glitchless mux — must disable before the source
   * frequency changes, then re-enable.  AUXSRC=0 selects clk_sys
   * which is now at PLL speed.
   *
   * The SDK requires a delay of ≥3 target clock cycles after disable
   * for ENABLE propagation.  At high ROSC speeds (up to 96 MHz on
   * RP2350 A3+) the back-to-back writes execute too quickly, causing
   * a clock glitch that corrupts the UART shift register state. */
  CLK_PERI_CTRL = 0;               /* disable */
  for (volatile int i = 0; i < 64; i++) ; /* wait for ENABLE propagation */
  CLK_PERI_CTRL = CLK_PERI_ENABLE; /* re-enable on clk_sys = PLL speed */

  /* No XIP cache flush needed: the QMI clock divider automatically
   * scales with clk_sys, so flash reads work correctly at PLL speed.
   * The .ramfunc placement of this function avoids any XIP dependency
   * during the clock transition itself. */
}
