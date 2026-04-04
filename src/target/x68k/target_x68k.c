/*
 * target_x68k.c — Target implementation for X68000
 *
 * Implements the target hook API (target.h) for the Sharp X68000 personal
 * computer.  Console I/O is provided by the IPL IOCS via TRAP #15.
 *
 * X68000 hardware:
 *   CPU:     Motorola 68000 @ 10 MHz
 *   RAM:     1 MB standard, up to 12 MB expanded
 *   Console: TVRAM text screen via IOCS _B_PUTC / _B_GETC / _B_KEYSNS
 *   Timer:   MFP (MC68901) Timer-C at 100 Hz (Phase X-2)
 *   Storage: 5-inch floppy (Phase X-3 UFS boot floppy)
 *
 * Boot flow:
 *   Stage1 (sector 0) → Stage2 (sectors 1–3) → kernel at 0x006000
 *   Stage2 copies .vectors to 0x000000, restores TRAP #15 to IPL IOCS,
 *   then jumps to Reset_Handler at 0x006400.
 *
 * NOTE (Phase X-3):
 *   - Preemptive scheduling via MFP Timer-C at 100 Hz
 *   - Rootfs is a UFS image (boot/rootfs.ufs) loaded into RAM by stage2
 *   - Kernel mounts it via flatblk ("ram0") as the initial root filesystem
 */

#include "../target.h"
#include "arch/arch.h"
#include "core/driver/uart.h"
#include "common/errno.h"
#include "vfs/tty.h"
#include "core/klog.h"
#include "core/mm/mem_region.h"
#include "core/mm/page.h"
#include "core/proc/proc.h"
#include "core/proc/sched.h"
#include "vfs/vfs.h"
#ifdef PPAP_HAS_BLKDEV
#include "core/driver/blkdev.h"
#include "core/driver/flatblk.h"
#include "vfs/ufs.h"
#endif
#include <stddef.h>
#include <stdint.h>

/* ── Timer driver ────────────────────────────────────────────────────── */

/* Defined in drivers/timer_x68k.c */
extern void timer_init(void);

/* Benign IRQ handler — silences unhandled hardware IRQs with a bare rte.
 * Defined in arch/m68k/irq.S (or similar). */
extern void m68k_irq_ignore(void);

/* ── TRAP #0 syscall dispatch ────────────────────────────────────────── *
 *
 * Called from m68k_trap0_handler (trap.S) with a pointer to the saved
 * register frame.  Register layout (Linux m68k / musl convention):
 *
 *   regs[0]  = d0  (syscall number → overwritten with return value)
 *   regs[1]  = d1  (arg 1)
 *   regs[2]  = d2  (arg 2)
 *   regs[3]  = d3  (arg 3)
 *   regs[4]  = d4  (arg 4)
 *   regs[5]  = d5  (arg 5)
 *   regs[8]  = a0  (arg 6)
 *
 * Maps the m68k register frame onto the shared syscall_dispatch() API.
 * ────────────────────────────────────────────────────────────────────── */

#include "core/signal/signal.h"
#include "core/syscall/syscall.h"

void m68k_syscall_entry(uint32_t *regs) {
  uint32_t nr = regs[0]; /* d0 = syscall number */
  uint32_t a4 = regs[5]; /* d5 = arg 5 */
  uint32_t a5 = regs[8]; /* a0 = arg 6 */

  uint32_t saved_d1 = regs[1];

  current->svc_needs_restart = 0;

  syscall_dispatch(&regs[1], nr, a4, a5);

  regs[0] = regs[1];

  while (current->svc_needs_restart) {
    current->svc_needs_restart = 0;
    regs[0] = nr;
    regs[1] = saved_d1;
    syscall_dispatch(&regs[1], nr, a4, a5);
    regs[0] = regs[1];
  }

  signal_check(regs);
}

/* ── Target hooks ────────────────────────────────────────────────────── */

void target_early_init(void) {
  /* Patch hardware interrupt vectors BEFORE the first IOCS call.
   *
   * Stage2 copies the kernel's .vectors to 0x000000 before jumping here,
   * so autovectors 25-30 (OPM, MFP, FDC, VSYNC, DMA …) and MFP vectored
   * interrupts 64-79 all point to Default_Handler (stop #0x2700) at this
   * point.  Some IOCS functions (e.g. _B_CLR_ST) temporarily lower the CPU
   * interrupt priority level to allow VSYNC-sync; if an interrupt fires
   * before the vectors are overridden the CPU would halt inside the IOCS.
   *
   * NOTE: timer_init() is called later in target_late_init() and installs
   * m68k_timer_isr at vt[69].  It must come AFTER this loop; the loop
   * filling vt[64-79] with m68k_irq_ignore runs here so that call is safe.
   */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
  volatile uint32_t *vt = (volatile uint32_t *)0x0;
  uint32_t ignore = (uint32_t)(uintptr_t)m68k_irq_ignore;

  /* Stage2 preserved ALL IPL ROM autovectors (24-31) and MFP vectors
   * (64-79) so that IOCS functions work correctly.  Here we only need
   * to replace remaining Default_Handler entries (vectors 10-23, 34-46,
   * 80-255) with m68k_irq_ignore to prevent CPU halts on unexpected
   * interrupts.
   *
   * Vectors already correctly set:
   *   0-9:   SSP, Reset, Bus/Address error, etc. (kernel handlers)
   *   24-31: Autovectors (IPL ROM — VSYNC, SCC, etc.)
   *   32:    TRAP #0 — syscall
   *   33:    TRAP #1 — yield
   *   47:    TRAP #15 — IOCS dispatch (IPL ROM)
   *   64-79: MFP vectored interrupts (IPL ROM — keyboard, timers)
   *
   * timer_init() overwrites vector 69 (Timer-C) with m68k_timer_isr
   * for preemptive scheduling.
   */
  for (uint32_t v = 10u; v < 24u; v++) vt[v] = ignore;
  for (uint32_t v = 34u; v < 47u; v++) /* TRAP #2-#14 */
    vt[v] = ignore;
  for (uint32_t v = 48u; v < 64u; v++) /* TRAP #16+ and reserved */
    vt[v] = ignore;
  for (uint32_t v = 80u; v < 256u; v++) /* extended vectors */
    vt[v] = ignore;
#pragma GCC diagnostic pop

  /* Diagnostic: "Po" — kernel reached (TRAP #15 = IPL IOCS, restored by
   * stage2). Together with stage2's "PiPA" this completes the "PiPAPo" banner.
   */
  {
    register uint32_t d0 asm("d0") = 0x20u;
    register uint32_t d1 asm("d1") = 'P';
    asm volatile("trap #15" : "+r"(d0) : "r"(d1) : "a0", "a1", "memory");
    d0 = 0x20u;
    d1 = (uint32_t)'o';
    asm volatile("trap #15" : "+r"(d0) : "r"(d1) : "a0", "a1", "memory");
  }
  uart_init();
  klog_set_logger(KLOG_LOGGER_PRIMARY, uart_putc, NULL);
  klog(" booting... [x68k]\n");
  klog("Console: X68000 IOCS (TVRAM)\n");
  klog("Phase X-2: preemptive scheduling (MFP Timer-C), embedded romfs\n");
}

extern int uart_serial_putc(char c, void (*notify)(void));

void target_late_init(void) {
  /* Vector patching was done in target_early_init() before the first IOCS
   * call.  Here we start MFP Timer-C for preemptive scheduling.
   * timer_init() overwrites vector 69 with m68k_timer_isr. */
  timer_init();

  /* Set up dual-TTY: TTY_DISPLAY = TVRAM (primary), TTY_SERIAL = RS-232C */
  extern int uart_serial_getc(void);
  extern int uart_serial_rx_avail(void);
  static const tty_backend_t tvram_be = {
      .putc = uart_putc,
      .getc = uart_getc,
      .rx_avail = uart_rx_avail,
      .get_cols = NULL,
      .get_rows = NULL,
  };
  static const tty_backend_t serial_be = {
      .putc = uart_serial_putc,
      .getc = uart_serial_getc,
      .rx_avail = uart_serial_rx_avail,
      .get_cols = NULL,
      .get_rows = NULL,
  };
  tty_set_backend(TTY_DISPLAY, &tvram_be);
  tty_set_backend(TTY_SERIAL, &serial_be);
  tty_set_console(TTY_DISPLAY);
  klog_set_logger(KLOG_LOGGER_SECONDARY, uart_serial_putc, NULL);

  /* Register input polls for both consoles.
   * Both use direct hardware register reads — NO IOCS TRAP #15 calls —
   * because IOCS functions hang when called from the idle thread context.
   *
   * TVRAM: uart_rx_avail_hw() checks IOCS key buffer byte count at $0812
   * Serial: uart_serial_rx_avail_hw() checks SCC channel B RR0 bit 0 */
  extern int uart_rx_avail_hw(void);
  extern int uart_serial_rx_avail_hw(void);
  sched_set_input_poll(uart_rx_avail_hw, TTY_DISPLAY);
  sched_set_input_poll2(uart_serial_rx_avail_hw, TTY_SERIAL);
}

int target_mount_rootfs(void) {
#ifdef PPAP_HAS_BLKDEV
  /* Derive rootfs address and size directly instead of relying on the
   * stage2 handoff at 0x002FF4-0x002FFC.  The IPL IOCS _B_READ handler
   * corrupts the 0x002000-0x003FFF region during floppy I/O, making the
   * handoff record unreliable.
   *
   * The rootfs address is always __page_pool_start (stage2 loads it there
   * via ROOTFS_BASE, matching the kernel's linker-provided symbol).
   * The rootfs size is derived from its own UFS superblock (block_count
   * × block_size), which stage2 loaded correctly to RAM. */
  uint32_t addr = (uint32_t)(uintptr_t)__page_pool_start;

  /* Validate rootfs: check UFS magic at the rootfs address */
  const uint32_t *sb = (const uint32_t *)(uintptr_t)addr;
  if (sb[0] != 0x55465331u) { /* UFS_MAGIC */
    klogf("x68k: no UFS magic at 0x%lx (got 0x%lx)\n", (unsigned long)addr,
          (unsigned long)sb[0]);
    return -1;
  }
  uint32_t block_size = sb[1];  /* s_block_size */
  uint32_t block_count = sb[2]; /* s_block_count */
  /* Compute size via shift: block_size is always a power of 2 for UFS.
   * Avoids potential 16-bit truncation in 68000 multiply codegen. */
  uint32_t bs_shift = 0;
  for (uint32_t bs = block_size; bs > 1u; bs >>= 1) bs_shift++;
  uint32_t size = block_count << bs_shift;
  klogf("x68k: ramdisk at %lx, %lu bytes (%lu blocks x %lu)\n",
        (unsigned long)addr, (unsigned long)size,
        (unsigned long)block_count, (unsigned long)block_size);

  /* Reserve the page-pool portion of the rootfs image so the allocator
   * never hands it out and overwrites the live UFS data. */
  {
    uintptr_t pool_base = page_pool_base();
    uintptr_t pool_end = pool_base + page_count * PAGE_SIZE;
    uintptr_t reserve_start = (uintptr_t)addr & ~(uintptr_t)(PAGE_SIZE - 1u);
    uintptr_t reserve_end =
        ((uintptr_t)addr + size + PAGE_SIZE - 1u) &
        ~(uintptr_t)(PAGE_SIZE - 1u);
    proc_image_segment_t reserved_rootfs;

    if (reserve_start < pool_base) reserve_start = pool_base;
    if (reserve_end > pool_end) reserve_end = pool_end;
    if (reserve_start < reserve_end &&
        mem_region_alloc_at(&reserved_rootfs, PPAP_MEM_RAM_DATA,
                            (void *)reserve_start,
                            reserve_end - reserve_start,
                            PROC_IMAGE_SEG_OWNED | PROC_IMAGE_SEG_WRITABLE) < 0)
      klog("x68k: rootfs reservation FAILED\n");
  }

  flatblk_init("ram0", (const void *)(uintptr_t)addr, size);
  blkdev_t *bd = blkdev_find("ram0");
  if (!bd) return -1;
  return vfs_mount("/", &ufs_ops, MNT_RDONLY, bd);
#else
  return -1;
#endif
}

void target_post_mount(void) {
  /* Nothing target-specific needed after rootfs mount */
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

const char *target_name(void) { return "x68k"; }

uint32_t target_caps(void) {
  return 0; /* Phase X-3: no SD, no SPI, no Core 1 */
}

/* Yield-test process — runs on its own stack, yields back to thread 0 */
void yield_test_process(void) {
  for (int i = 0; i < 4; i++) sched_switch();

  current->state = PROC_FREE;
  for (;;) sched_switch();
}
