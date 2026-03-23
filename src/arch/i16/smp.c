/*
 * smp.c -- SMP stubs for i8086 (single-core)
 */

void core1_launch(void (*entry)(void)) { (void)entry; }
void core1_sched_entry(void) {}
