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

/* Maximum argv / envp entries accepted by execve (excluding terminating
 * NULL).  The two caps are independent because argv and envp have
 * different typical profiles: argv is a handful of short tokens, while
 * envp carries more numerous (but still bounded) NAME=VALUE entries.
 * Byte budgets are tuned per target inside sys_proc.c, but the entry
 * counts are part of the userspace-visible contract and stay shared. */
#define EXEC_ARGV_MAX 64
#define EXEC_ENVP_MAX 32

/*
 * execve — Load an ELF binary and set up a process to execute it.
 *
 * Looks up the path captured in `args` in the VFS, validates the ELF
 * header, copies code/data segments to SRAM (at their linked addresses),
 * allocates a stack page, and initialises the PCB's stack frame so
 * PendSV can restore it.
 *
 * `args` carries the (path, argv[], envp[]) triple in a data-region
 * page.  Callers must populate it (path is required; argv[0]
 * defaulting to the basename of path, when argc == 0, is the caller's
 * responsibility before calling).  Loaders read argv/envp through the
 * exec_args_* accessors and may ignore envp when their personality
 * has no environment concept.
 *
 * On success: returns 0.  The PCB is ready; caller sets state = RUNNABLE.
 * On failure: returns negative errno, PCB is unchanged.
 */
struct exec_args;
int exec_execve(pcb_t *p, const struct exec_args *args);

/*
 * exec_execve_simple — Convenience wrapper for callers that only know
 * a path (init, standalone kernel-spawned processes).  Allocates an
 * args page, populates path + default argv[0] = path + empty envp,
 * calls exec_execve, and frees the args page.  Returns 0 on success,
 * -errno on failure.
 */
int exec_execve_simple(pcb_t *p, const char *path);

#endif /* PPAP_KERNEL_CORE_EXEC_EXEC_H */
