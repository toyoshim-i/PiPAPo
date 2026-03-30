/*
 * mod_exec.inc — Exec module function index
 *
 * Single source of truth for function names and their indices.
 * Define MOD_EXEC_ENTRY(name, idx) before including this file.
 */

MOD_EXEC_ENTRY(do_execve, 0)

#ifndef MOD_EXEC_FUNC_COUNT
#define MOD_EXEC_FUNC_COUNT 1
#endif
