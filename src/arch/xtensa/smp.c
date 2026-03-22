/*
 * smp.c — SMP stubs for Xtensa (single-core initial port)
 *
 * ESP32-S3 has dual LX7 cores.  Dual-core support will be added
 * in a future phase using the crosscore interrupt mechanism.
 */

void core1_launch(void (*entry)(void)) { (void)entry; }
void core1_sched_entry(void) {}
