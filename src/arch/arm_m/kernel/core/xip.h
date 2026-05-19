/*
 * xip.h — RP-style XIP verification and benchmark (arm_m only)
 *
 * Called from arm_m/.../mem_helper.c's mem_helper_post_init under
 * PPAP_TESTS:
 *   1. Prints the runtime address of xip_add — visible in the XIP window
 *      (0x10001xxx on real hardware).
 *   2. Runs xip_add(3,4) and checks the result is 7.
 *   3. Benchmarks the same summation loop from XIP flash (xip_bench) and
 *      from SRAM (sram_bench) using the Cortex-M SysTick counter, then
 *      prints both cycle counts so the XIP cache benefit is visible.
 *
 * The .text.xip_test section that holds xip_add / xip_bench is placed
 * in the RP2040 XIP flash window by each pico-target's linker script.
 * qemu_arm has no real XIP flash but runs the same correctness check.
 */

#ifndef PPAP_ARCH_ARM_M_KERNEL_CORE_XIP_H
#define PPAP_ARCH_ARM_M_KERNEL_CORE_XIP_H

#include <stdint.h>

/* Run all XIP diagnostics: address probe, correctness check, and benchmarks.
 * Called from mm_init() after the page pool is set up. */
void xip_verify(void);

/* Exposed so tests can call them directly if needed. */
int xip_add(int a, int b);
uint32_t xip_bench(uint32_t n);
uint32_t sram_bench(uint32_t n);

#endif /* PPAP_ARCH_ARM_M_KERNEL_CORE_XIP_H */
