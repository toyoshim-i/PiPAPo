/*
 * smp.c — SMP stubs for RISC-V (single-core).  Dual-core via RP2350 SIO
 * mailbox is a future enhancement.
 */

void core1_launch(void (*entry)(void)) { (void)entry; }
void core1_sched_entry(void) {}
