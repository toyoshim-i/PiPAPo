/*
 * timer_qemu_m68k.c — Goldfish RTC timer driver for QEMU virt m68k
 *
 * Uses the Goldfish RTC #0 at 0xFF006000 as a periodic timer for the
 * scheduler.  The RTC provides an alarm mechanism: we set an alarm
 * 10 ms in the future, and when it fires we clear the interrupt,
 * schedule the next alarm, and call sched_timer_tick().
 *
 * Interrupt routing:
 *   Goldfish RTC #0 → Goldfish PIC #6 (GPIO 0) → CPU IRQ Level 6
 *   → Autovector 31 (vector table offset 0x7C)
 *
 * The timer ISR is in switch.S (m68k_timer_isr).
 */

#include <stdint.h>

#include "kernel/core/klog.h"
#include "kernel/core/proc/sched.h"

/* ── Goldfish RTC #0 registers ───────────────────────────────────────────── */

#define GF_RTC_BASE 0xFF006000u

#define RTC_TIME_LOW (*(volatile uint32_t *)(GF_RTC_BASE + 0x00u))
#define RTC_TIME_HIGH (*(volatile uint32_t *)(GF_RTC_BASE + 0x04u))
#define RTC_ALARM_LOW (*(volatile uint32_t *)(GF_RTC_BASE + 0x08u))
#define RTC_ALARM_HIGH (*(volatile uint32_t *)(GF_RTC_BASE + 0x0Cu))
#define RTC_IRQ_ENABLED (*(volatile uint32_t *)(GF_RTC_BASE + 0x10u))
#define RTC_CLEAR_ALARM (*(volatile uint32_t *)(GF_RTC_BASE + 0x14u))
#define RTC_ALARM_STATUS (*(volatile uint32_t *)(GF_RTC_BASE + 0x18u))
#define RTC_CLEAR_IRQ (*(volatile uint32_t *)(GF_RTC_BASE + 0x1Cu))

/* ── Goldfish PIC #6 registers ───────────────────────────────────────────── */

#define GF_PIC6_BASE 0xFF005000u

#define PIC6_REG_ENABLE (*(volatile uint32_t *)(GF_PIC6_BASE + 0x10u))

/* ── Timer period ────────────────────────────────────────────────────────── */

/* 10 ms in nanoseconds (100 Hz tick rate, matching ARM SysTick) */
#define TIMER_PERIOD_NS 10000000u

/* ── Internal helper ─────────────────────────────────────────────────────── */

/* Read the current RTC time as a 64-bit nanosecond value and schedule
 * the next alarm TIMER_PERIOD_NS in the future. */
static void timer_set_next_alarm(void) {
  /* Read TIME_LOW first, then TIME_HIGH (RTC latches high on low read) */
  uint32_t lo = RTC_TIME_LOW;
  uint32_t hi = RTC_TIME_HIGH;
  uint64_t now = ((uint64_t)hi << 32) | lo;
  uint64_t next = now + TIMER_PERIOD_NS;

  /* Write ALARM_HIGH first, then ALARM_LOW (ALARM_LOW triggers set) */
  RTC_ALARM_HIGH = (uint32_t)(next >> 32);
  RTC_ALARM_LOW = (uint32_t)(next & 0xFFFFFFFFu);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void timer_init(void) {
  /* Enable the RTC interrupt in Goldfish PIC #6 (bit 0 = RTC #0) */
  PIC6_REG_ENABLE = 1u;

  /* Schedule the first alarm */
  timer_set_next_alarm();

  /* Enable the RTC IRQ */
  RTC_IRQ_ENABLED = 1u;

  /* Timer ready — no boot log (consistent with ARM SysTick) */
}

/* ── Timer ISR C handler ─────────────────────────────────────────────────── *
 *
 * Called from m68k_timer_isr (switch.S) with interrupts masked.
 * Clears the pending interrupt, schedules the next alarm, and calls
 * the shared scheduler tick handler.
 * ────────────────────────────────────────────────────────────────────────── */

void m68k_timer_handler_c(void) {
  /* Acknowledge the interrupt (clear pending flag in RTC) */
  RTC_CLEAR_IRQ = 1u;

  /* Schedule the next periodic alarm */
  timer_set_next_alarm();

  /* Tick the scheduler (always supervisor mode for now) */
  sched_timer_tick(0);
}
