/*
 * xip_test.h — XIP (Execute-In-Place) verification and benchmark
 *
 * Step 9: verify that code runs correctly from XIP flash and measure
 * cache effectiveness by comparing a flash-resident loop against the
 * same loop copied to SRAM at startup.
 */

#ifndef PPAP_XIP_TEST_H
#define PPAP_XIP_TEST_H

#include <stdint.h>

/* Simple addition — placed in the ".text.xip_test" subsection so its address
 * is easily identifiable as XIP-flash (0x10001xxx).
 * Call xip_add(3, 4) and check the result is 7. */
int xip_add(int a, int b);

/* Run a tight summation loop of N iterations from XIP flash and return the
 * elapsed SysTick cycles (processor clock, 133 MHz after Step 7). */
uint32_t xip_bench(uint32_t n);

/* Same loop but copied to SRAM by Reset_Handler at startup (placed in
 * ".data.sram_bench").  Compare with xip_bench() to gauge cache benefit. */
uint32_t sram_bench(uint32_t n);

#endif /* PPAP_XIP_TEST_H */
