/*
 * smp.c — SMP stubs for RISC-V (single-core initial port)
 *
 * Phase RV-6 will implement dual-core support using RP2350 SIO mailbox.
 */

void core1_launch(void (*entry)(void)) { (void)entry; }
void core1_sched_entry(void) {}
