/*
 * mod_exec.h — Exec kernel module interface
 *
 * Defines the module boundary for program loading and execution.
 * The exec module loads binary files from the VFS, identifies their
 * format, and sets up process memory and entry point.
 *
 * Usage:
 *   #include "common/mod/mod_exec.h"
 *   int err = mod_exec.do_execve(proc, "/sbin/init", NULL);
 *
 * Implementation: src/kernel/exec/exec.c, src/kernel/exec/loader.c
 */

#ifndef PPAP_KERNEL_MOD_MOD_EXEC_H
#define PPAP_KERNEL_MOD_MOD_EXEC_H

#include "module.h"

/* Forward declaration */
struct pcb;
typedef struct pcb pcb_t;

MOD_DECLARE_BEGIN(exec)

  /*
   * do_execve — Load and execute a binary file.
   *
   *   p     Target process (PCB). Must have stack_page allocated.
   *   path  Filesystem path to the executable (e.g. "/sbin/init").
   *   argv  NULL-terminated argument array, or NULL for no args.
   *
   * Resolves the file via mod_vfs.lookup(), reads it into memory,
   * detects the binary format (ELF, flat, .COM), and delegates to
   * the matching loader which sets up the process entry point,
   * stack, and user pages.
   *
   * On success: p->sp, p->user_pages, p->comm are set. Returns 0.
   * On failure: returns negative errno (-ENOENT, -ENOEXEC, -ENOMEM).
   */
  MOD_FUNC(exec, int, do_execve, pcb_t *, const char *,
                                  const char * const *)

MOD_DECLARE_END(exec)

#define MOD_EXEC_ENTRY(name, idx) /* count only */
#include "mod_exec.inc"
#undef MOD_EXEC_ENTRY
_Static_assert(sizeof(mod_exec_t) == MOD_EXEC_FUNC_COUNT * sizeof(void (*)(void)),
               "mod_exec_t size mismatch — update MOD_EXEC_FUNC_COUNT in "
               "mod_exec.inc");

#endif /* PPAP_KERNEL_MOD_MOD_EXEC_H */
