/*
 * xip.c — XIP (Execute-In-Place) verification and SysTick benchmark
 *
 * xip_verify() is called from mm_init() and:
 *   1. Prints the runtime address of xip_add — on real hardware this should
 *      be in the XIP flash window (0x10001xxx).
 *   2. Runs xip_add(3,4) and checks the result is 7.
 *   3. Benchmarks the same summation loop executing from XIP flash
 *      (xip_bench) and from SRAM (sram_bench) using the Cortex-M SysTick
 *      counter, then prints both cycle counts so the cache benefit is visible.
 *
 * xip_add()    — minimal function in .text.xip_test; its address proves XIP
 *                flash execution (0x10001xxx on real hardware).
 *
 * xip_bench()  — tight summation loop measured with SysTick; executes from
 *                XIP flash.
 *
 * sram_bench() — identical loop placed in .ramfunc.sram_bench; Reset_Handler
 *                copies the entire .ramfunc section (flash LMA → SRAM VMA)
 *                at startup, so this function runs from SRAM.  Comparing its
 *                cycle count with xip_bench() shows the XIP cache benefit.
 *
 * SysTick is a 24-bit down-counter clocked from the processor (133 MHz).
 * At 133 MHz, 0xFFFFFF ticks ≈ 126 ms — more than enough for short benchmarks.
 */

#include "xip.h"
#include "drivers/uart.h"
#include <stdint.h>

/* ==========================================================================
 * SysTick — ARM Cortex-M0+ system timer (PPB, always accessible)
 * ========================================================================== */

#define REG(addr)  (*(volatile uint32_t *)(uintptr_t)(addr))

#define SYST_CSR  REG(0xE000E010u)  /* Control and Status Register */
#define SYST_RVR  REG(0xE000E014u)  /* Reload Value Register (24-bit) */
#define SYST_CVR  REG(0xE000E018u)  /* Current Value Register (counts down) */

/* CSR bits */
#define SYST_CSR_CLKSOURCE_CPU  (1u << 2)  /* use processor clock (not ref clock) */
#define SYST_CSR_ENABLE         (1u << 0)

/* 24-bit maximum reload / mask */
#define SYST_MAX  0x00FFFFFFu

/* ==========================================================================
 * XIP functions
 * ========================================================================== */

/*
 * xip_add — placed in .text.xip_test so its address (printed by xip_verify)
 * is visibly in the XIP flash window (0x10001xxx on real hardware).
 */
__attribute__((section(".text.xip_test"), noinline))
int xip_add(int a, int b)
{
    return a + b;
}

/*
 * xip_bench — runs from XIP flash; measures a tight summation loop with
 * SysTick.  Returns elapsed processor-clock cycles.
 */
__attribute__((section(".text.xip_test"), noinline))
uint32_t xip_bench(uint32_t n)
{
    SYST_RVR = SYST_MAX;   /* set reload to max */
    SYST_CVR = 0;          /* writing any value clears CVR and the COUNTFLAG */
    SYST_CSR = SYST_CSR_CLKSOURCE_CPU | SYST_CSR_ENABLE;

    volatile uint32_t sum = 0;
    for (uint32_t i = 0; i < n; i++)
        sum += i;
    (void)sum;

    uint32_t elapsed = (SYST_MAX - SYST_CVR) & SYST_MAX;
    SYST_CSR = 0;          /* stop SysTick */
    return elapsed;
}

/*
 * sram_bench — same loop, placed in .ramfunc.sram_bench so Reset_Handler
 * copies the machine code to SRAM before kmain() runs.  When called, the
 * CPU executes from SRAM (VMA) rather than flash (LMA).
 *
 * This works because:
 *   1. The function + its literal pool are contiguous in .ramfunc.
 *   2. Reset_Handler copies the entire .ramfunc section (flash LMA → SRAM VMA).
 *   3. The linker assigns the SRAM VMA to the symbol, so a direct call
 *      branches to the SRAM copy.
 *   4. PC-relative literals (absolute register addresses) remain valid
 *      because both instructions and literals shift by the same offset.
 */
__attribute__((section(".ramfunc.sram_bench"), noinline))
uint32_t sram_bench(uint32_t n)
{
    SYST_RVR = SYST_MAX;
    SYST_CVR = 0;
    SYST_CSR = SYST_CSR_CLKSOURCE_CPU | SYST_CSR_ENABLE;

    volatile uint32_t sum = 0;
    for (uint32_t i = 0; i < n; i++)
        sum += i;
    (void)sum;

    uint32_t elapsed = (SYST_MAX - SYST_CVR) & SYST_MAX;
    SYST_CSR = 0;
    return elapsed;
}

/* ==========================================================================
 * xip_verify — called from mm_init()
 * ========================================================================== */

void xip_verify(void)
{
    /* Address probe: on real hardware xip_add lives in XIP flash (0x10001xxx) */
    uart_puts("XIP: xip_add @ ");
    uart_print_hex32((uint32_t)(uintptr_t)xip_add);
    uart_puts("\n");

    /* Correctness check */
    int result = xip_add(3, 4);
    uart_puts("XIP: xip_add(3,4) = ");
    uart_putc('0' + (char)result);
    uart_puts(result == 7 ? " OK\n" : " FAIL\n");

    /* Benchmark: compare XIP flash vs SRAM execution speed */
    uint32_t flash_cyc = xip_bench(10000);
    uint32_t sram_cyc  = sram_bench(10000);
    uart_puts("XIP: flash bench(10000) = ");
    uart_print_hex32(flash_cyc);
    uart_puts(" cycles\n");
    uart_puts("XIP: sram  bench(10000) = ");
    uart_print_hex32(sram_cyc);
    uart_puts(" cycles\n");
}
