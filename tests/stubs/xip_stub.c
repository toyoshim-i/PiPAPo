/*
 * xip_stub.c — Host-side stubs for the XIP verification module
 *
 * Replaces src/kernel/mm/xip.c when building tests on the host.
 * xip.c accesses SysTick hardware registers (0xE000E010–0xE000E018)
 * and uses ARM-specific section attributes; none of this is portable.
 *
 * xip_verify() is called from mm_init(); here it's a no-op.
 * xip_add() returns the correct sum so any test calling it still works.
 */

#include <stdint.h>

void     xip_verify(void)           {}
int      xip_add(int a, int b)      { return a + b; }
uint32_t xip_bench(uint32_t n)      { (void)n; return 0; }
uint32_t sram_bench(uint32_t n)     { (void)n; return 0; }
