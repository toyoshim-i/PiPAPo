/*
 * target_ibmpc.c -- IBM PC target: P-2 standalone scheduler
 *
 * Self-contained mini-scheduler that proves preemptive context switching
 * works on 8086 real mode.  Does NOT link the full PPAP kernel — that
 * will come in P-3 after the 16-bit/32-bit portability work.
 *
 * Two test processes print alternating characters ('A' and 'B') to
 * serial.  Switching is purely timer-driven (PIT IRQ 0 at 100 Hz).
 */

#include <stdint.h>
#include "drivers/uart.h"
#include "drivers/bios_con.h"
#include "drivers/timer_pit.h"

/* ── Console output ─────────────────────────────────────────────────────── */

static void klog(const char *msg)
{
  while (*msg) {
    uart_putc(*msg, 0);
    bios_putc(*msg);
    msg++;
  }
}

/* ── Minimal PCB ────────────────────────────────────────────────────────── */

#define PROC_MAX   3
#define TIME_SLICE 10   /* ticks per slice (100 ms at 100 Hz) */

enum { PROC_FREE = 0, PROC_RUNNABLE, PROC_RUNNING };

typedef struct {
  uint16_t sp;           /* saved stack pointer */
  uint16_t state;
  uint16_t ticks_left;
} i16_pcb_t;

i16_pcb_t i16_procs[PROC_MAX];

/* Exported for switch.S */
i16_pcb_t *i16_current;

/* From i16_common.c */
extern volatile uint16_t i16_switch_pending;
extern volatile uint32_t i16_tick_count;

/* From i16_common.c */
extern uint32_t *arch_build_initial_frame(uint32_t *sp, void (*entry)(void));

/* ── Scheduler (called from switch.S) ───────────────────────────────────── */

/*
 * sched_next() -- round-robin selection.
 * Returns pointer to next runnable PCB.  Called with interrupts disabled.
 */
i16_pcb_t *sched_next(void)
{
  uint16_t cur = (uint16_t)(i16_current - i16_procs);
  uint16_t next = cur;

  for (uint16_t i = 0; i < PROC_MAX; i++) {
    next = (next + 1) % PROC_MAX;
    if (i16_procs[next].state == PROC_RUNNABLE) {
      i16_current->state = PROC_RUNNABLE;
      i16_procs[next].state = PROC_RUNNING;
      i16_procs[next].ticks_left = TIME_SLICE;
      return &i16_procs[next];
    }
  }

  /* No other runnable process — stay on current */
  i16_current->ticks_left = TIME_SLICE;
  return i16_current;
}

/*
 * sched_timer_tick() -- called from i16_timer_handler_c() in the ISR.
 */
static void sched_timer_tick(void)
{
  i16_tick_count++;

  if (i16_current->ticks_left > 0)
    i16_current->ticks_left--;

  if (i16_current->ticks_left == 0)
    i16_switch_pending = 1;
}

/* Called from switch.S */
void i16_timer_handler_c(void)
{
  sched_timer_tick();
}

/* ── Test processes ──────────────────────────────────────────────────────── */

static void busy_wait(void)
{
  for (volatile uint16_t i = 0; i < 20000; i++)
    ;
}

static void proc_a(void)
{
  for (;;) {
    uart_putc('A', 0);
    busy_wait();
  }
}

static void proc_b(void)
{
  for (;;) {
    uart_putc('B', 0);
    busy_wait();
  }
}

/* ── Process stacks ──────────────────────────────────────────────────────── */

static uint8_t stack_a[1024];
static uint8_t stack_b[1024];

/* ── kmain ──────────────────────────────────────────────────────────────── */

void kmain(void)
{
  uart_init();
  klog("PiPAPo booting... [ibmpc]\r\n");

  /* Set up process A */
  uint16_t *sp_a = (uint16_t *)(uintptr_t)arch_build_initial_frame(
      (uint32_t *)(uintptr_t)&stack_a[sizeof(stack_a)], proc_a);
  i16_procs[1].sp = (uint16_t)(uintptr_t)sp_a;
  i16_procs[1].state = PROC_RUNNABLE;
  i16_procs[1].ticks_left = TIME_SLICE;

  /* Set up process B */
  uint16_t *sp_b = (uint16_t *)(uintptr_t)arch_build_initial_frame(
      (uint32_t *)(uintptr_t)&stack_b[sizeof(stack_b)], proc_b);
  i16_procs[2].sp = (uint16_t)(uintptr_t)sp_b;
  i16_procs[2].state = PROC_RUNNABLE;
  i16_procs[2].ticks_left = TIME_SLICE;

  /* Process 0 = idle (current context becomes idle when we switch away) */
  i16_procs[0].state = PROC_RUNNING;
  i16_procs[0].ticks_left = 1; /* expire immediately to switch to proc A */
  i16_current = &i16_procs[0];

  /* Initialize PIT + PIC + install ISR */
  klog("PIT: 100 Hz timer started\r\n");
  timer_init();

  /* Enable interrupts — the first timer tick will preempt us and
   * switch to proc A (since idle's ticks_left = 1). */
  __asm__ volatile ("sti");

  /* Idle loop */
  for (;;)
    __asm__ volatile ("hlt");
}
