/*
 * smp.c — Single-core SMP stubs for m68k targets.
 *
 * The X68000 and other m68k platforms are single-core, so all SMP
 * functions are no-ops.  This keeps the shared kernel code calling
 * core1_launch() / sio_fifo_push() / sio_fifo_pop() unconditionally.
 */

#include "core/cpu/smp.h"

void core1_launch(void (*entry)(void)) { (void)entry; }
void core1_sched_entry(void) {}
void sio_fifo_push(uint32_t value) { (void)value; }
uint32_t sio_fifo_pop(void) { return 0; }
