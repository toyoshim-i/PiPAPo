/*
 * xip.h — XIP (Execute-In-Place) verification and benchmark
 *
 * xip_verify() is integrated into mm_init() and:
 *   1. Prints the runtime address of xip_add — visible in the XIP window
 *      (0x10001xxx on real hardware).
 *   2. Runs xip_add(3,4) and checks the result is 7.
 *   3. Benchmarks the same summation loop from XIP flash (xip_bench) and
 *      from SRAM (sram_bench) using the Cortex-M SysTick counter, then
 *      prints both cycle counts so the XIP cache benefit is visible.
 */

#ifndef PPAP_MM_XIP_H
#define PPAP_MM_XIP_H

#include <stdint.h>

/* Run all XIP diagnostics: address probe, correctness check, and benchmarks.
 * Called from mm_init() after the page pool is set up. */
void xip_verify(void);

/* Exposed so tests can call them directly if needed. */
int      xip_add(int a, int b);
uint32_t xip_bench(uint32_t n);
uint32_t sram_bench(uint32_t n);

#endif /* PPAP_MM_XIP_H */
