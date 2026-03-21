/*
 * target_pico2.c — Target implementation for official Raspberry Pi Pico 2
 *
 * Pico 2: RP2350, 4 MB flash, no SD card, dual-core (Cortex-M33).
 * No SPI bus — omits spi_init() and sd_init().
 */

#include "../target.h"
#include "arch/arm_m/ioregs.h"
#include "config.h"
#include "drivers/arch/arm_m/uart_rpico.h"
#include "drivers/clock.h"
#include "drivers/uart.h"
#include "klog.h"
#include "mm/mpu.h"
#include "pico2.h"

#ifdef PPAP_TESTS
#include "ktest.h"
#endif

/* Linker-provided symbols for NSC veneer region (pico2.ld) */
extern char __nsc_veneer_start[];
extern char __nsc_veneer_end[];

/* ── SAU / TrustZone initialisation ───────────────────────────────────────
 *
 * RP2350 boots in Secure world (picobin IMAGE_TYPE = 0x1021).  The IDAU
 * maps 0x10/0x20 prefixes as Secure, 0x00/0x30 as Non-Secure aliases.
 *
 * TZ-2a strategy: enable SAU with ALLNS=0 and no regions.  This makes
 * SAU default everything to Secure; the IDAU then provides NS access for
 * the 0x00/0x30 alias addresses.  Result:
 *
 *   - Kernel at 0x10xxxxxx / 0x20xxxxxx → Secure (both SAU and IDAU agree)
 *   - User pages via 0x30xxxxxx alias   → NS     (IDAU overrides SAU)
 *
 * No SAU regions are needed for basic S/NS partitioning — the IDAU
 * address aliases handle it.  SAU regions will be added in TZ-2b for
 * the NSC syscall gateway.
 */
static void sau_init(void) {
  uint32_t nregions = SAU_TYPE & 0xFFu;

  /* Allow Non-Secure access to CP10+CP11 (FPU) — required for NS
   * user processes that use floating-point instructions. */
  NSACR |= (1u << 10) | (1u << 11);

  /* Route BusFault / HardFault / NMI to Secure world so they always
   * land in our existing handlers regardless of caller's security state. */
  SCB_AIRCR = AIRCR_VECTKEY | (SCB_AIRCR & ~AIRCR_BFHFNMINS);

  /* Region 0: NSC veneer — syscall gateway callable from Non-Secure.
   * __nsc_veneer_start / __nsc_veneer_end are defined by pico2.ld.
   * RBAR = base address (32-byte aligned).
   * RLAR = limit | ENABLE | NSC. */
  SAU_RNR = 0;
  SAU_RBAR = (uint32_t)&__nsc_veneer_start;
  SAU_RLAR = (uint32_t)&__nsc_veneer_end | SAU_RLAR_ENABLE | SAU_RLAR_NSC;

  /* Enable SAU with ALLNS=0.  Addresses not in any SAU region default
   * to Secure; IDAU provides NS for 0x00/0x30/0x50 alias addresses. */
  SAU_CTRL = SAU_CTRL_ENABLE;

  klogf("SAU: enabled (%u regions, 1 configured: NSC @ 0x%08x)\n", nregions,
        (unsigned)&__nsc_veneer_start);
}

/* ── Per-core Secure PSP stacks ──────────────────────────────────────────
 *
 * When an NS process is running, PSP_S must point to a valid Secure stack
 * so that the NSC gateway's SVC exception has somewhere to push its frame.
 * Each core gets its own 512-byte stack (sufficient for the 8-word HW
 * exception frame plus SVC_Handler's MSP usage is separate). */
static uint8_t secure_psp_stack[2][512] __attribute__((aligned(8)));
uint32_t secure_psp_top[2];

void target_early_init(void) {
  uart_init();
  klog("PiPAPo booting... [pico2]\n");
  klog("UART: 115200 bps @ 12 MHz XOSC\n");
  uart_tx_drain();   /* drain at 12 MHz; also disables UART0 NVIC */
  clock_init_pll();  /* switch clk_sys to PLL (PPAP_SYS_HZ)      */
  uart_reinit_pll(); /* set PLL-speed baud divisors               */
  klogf("System clock: %u MHz\n", PPAP_SYS_HZ / 1000000u);
  /* No SPI init — pico2 has no SD card slot */
}

void target_late_init(void) {
  /* No SD card to initialize */
  while (uart_getc() >= 0)
    ; /* drain boot noise from RX ring */
  mpu_init();
  sau_init();

  /* Initialize per-core Secure PSP stacks for NS gateway SVC */
  secure_psp_top[0] = (uint32_t)(uintptr_t)&secure_psp_stack[0][512];
  secure_psp_top[1] = (uint32_t)(uintptr_t)&secure_psp_stack[1][512];

  /* core1_launch moved to kmain — must run after init gets PID 1 */
}

void target_post_mount(void) {
#ifdef PPAP_TESTS
  ktest_run_all();
#endif
}

const char *target_init_path(void) {
#ifdef PPAP_TESTS
#ifdef PPAP_TESTS_EXTENDED
  return "/bin/runtests_ext";
#else
  return "/bin/runtests";
#endif
#else
  return "/sbin/init";
#endif
}

const char *target_name(void) { return "pico2"; }

uint32_t target_caps(void) {
  return TARGET_CAP_CORE1 | TARGET_CAP_REALUART;
  /* No TARGET_CAP_SD, no TARGET_CAP_SPI */
}

uint32_t target_ns_addr_xor(void) { return RP2350_NS_BIT; }

/* ── ARM FPB hardware breakpoints (native ptrace backend) ───────────────── *
 *
 * Cortex-M33 (ARMv8-M) uses FPB v2 with a completely different comparator
 * encoding than Cortex-M0+ (ARMv6-M, FPB v1).  The REPLACE field at
 * bits [31:30] is gone; address bits and a MATCH field at [2:1] replace it.
 * Implementing FPB v2 properly is future work — for now, report 0 slots
 * so the ptrace layer hides the HW_BP capability on this target.
 */

uint32_t target_debug_hwbp_slots(void) { return 0; }

int target_debug_hwbp_set(uint32_t slot, uint32_t addr) {
  (void)slot;
  (void)addr;
  return -1;
}

int target_debug_hwbp_clear(uint32_t slot) {
  (void)slot;
  return -1;
}
