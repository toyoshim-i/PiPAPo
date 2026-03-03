/*
 * ktest.h — Kernel integration test runner
 *
 * Only compiled when PPAP_TESTS=ON.  Called from target_post_mount()
 * after VFS + fstab mount, before sched_start().
 */

#ifndef PPAP_KTEST_H
#define PPAP_KTEST_H

void ktest_run_all(void);

#endif /* PPAP_KTEST_H */
