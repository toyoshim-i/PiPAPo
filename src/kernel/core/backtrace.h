/*
 * backtrace.h — Arch-overridable stack backtrace
 *
 * Best-effort frame-pointer walk used by diagnostic paths (page_free
 * double-free, future panic dumps).  Default impl is empty; arches
 * with a stable FP convention provide a strong override that prints a
 * short chain via mod_vfs.klogf.
 */

#ifndef PPAP_KERNEL_CORE_BACKTRACE_H
#define PPAP_KERNEL_CORE_BACKTRACE_H

void stack_backtrace(void);

#endif /* PPAP_KERNEL_CORE_BACKTRACE_H */
