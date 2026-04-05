/*
 * exec.h — ELF binary loader for PPAP
 *
 * execve() loads an ELF binary from the VFS (romfs flash) into SRAM
 * and prepares a PCB to run it.  The caller must set the process state
 * to PROC_RUNNABLE after a successful return.
 */

#ifndef PPAP_KERNEL_CORE_EXEC_EXEC_H
#define PPAP_KERNEL_CORE_EXEC_EXEC_H

#include "kernel/core/proc/proc.h"

/* Maximum argv entries accepted by execve (excluding terminating NULL). */
#define EXEC_ARGV_MAX 64

/*
 * execve — Load an ELF binary and set up a process to execute it.
 *
 * Looks up `path` in the VFS, validates the ELF header, copies code/data
 * segments to SRAM (at their linked addresses), allocates a stack page,
 * and initialises the PCB's stack frame so PendSV can restore it.
 *
 * argv: NULL-terminated argument array.  If NULL, defaults to {path, NULL}.
 *
 * On success: returns 0.  The PCB is ready; caller sets state = RUNNABLE.
 * On failure: returns negative errno, PCB is unchanged.
 */
int exec_execve(pcb_t *p, const char *path, const char *const *argv);

#endif /* PPAP_KERNEL_CORE_EXEC_EXEC_H */
