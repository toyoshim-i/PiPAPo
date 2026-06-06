/*
 * target_qemu_rv32.c — Target implementation for QEMU RISC-V virt machine
 *
 * QEMU virt: NS16550A UART, CLINT timer, no SD/SPI/display.
 * Used for CI testing and debugging the RISC-V port without hardware.
 */

#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/timer.h"
#include "target/target.h"

/* sifive_test device — built into QEMU virt rv32 */
#define SIFIVE_TEST_BASE 0x100000u
#define SIFIVE_TEST_PASS 0x5555u
#define SIFIVE_TEST_FAIL 0x3333u

void target_early_init(void) {
  /* Boot banner printed from klog_init_logger() (VFS side) */
}

void target_late_init(void) {
  riscv_timer_init();
  mod_vfs.notify(VFS_EVENT_LATE_INIT);
}

void target_post_mount(void) {
  /* No additional mounts on this target.
   * TODO: kernel tests crash rv32 (blkdev tests expect FAT32 ramblk) */
}

const char *target_name(void) { return "qemu_rv32"; }

uint32_t target_caps(void) {
  return TARGET_CAP_REALUART;
}

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

/*
 * QEMU poweroff via sifive_test device.
 *
 * Writing 0x5555 triggers FINISHER_PASS (QEMU exits 0).
 * Writing 0x3333 triggers FINISHER_FAIL (QEMU exits 1).
 */
void target_may_poweroff(uint8_t status) {
  volatile uint32_t *test = (volatile uint32_t *)SIFIVE_TEST_BASE;
  *test = status ? SIFIVE_TEST_FAIL : SIFIVE_TEST_PASS;
  for (;;) __asm__ volatile("nop");
}
