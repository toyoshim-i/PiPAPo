/*
 * exec.h — ELF binary loader for PPAP
 *
 * do_execve() loads an ELF binary from the VFS (romfs flash) into SRAM
 * and prepares a PCB to run it.  The caller must set the process state
 * to PROC_RUNNABLE after a successful return.
 */

#ifndef PPAP_EXEC_EXEC_H
#define PPAP_EXEC_EXEC_H

#include "kernel/proc/proc.h"

/*
 * do_execve — Load an ELF binary and set up a process to execute it.
 *
 * Looks up `path` in the VFS, validates the ELF header, copies code/data
 * segments to SRAM (at their linked addresses), allocates a stack page,
 * and initialises the PCB's stack frame so PendSV can restore it.
 *
 * On success: returns 0.  The PCB is ready; caller sets state = RUNNABLE.
 * On failure: returns negative errno, PCB is unchanged.
 */
int do_execve(pcb_t *p, const char *path);

#endif /* PPAP_EXEC_EXEC_H */
