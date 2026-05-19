/*
 * backtrace.c — Weak default: no backtrace available
 *
 * Arches that can walk their frame chain (currently riscv) supply a
 * strong override in src/arch/<arch>/kernel/core/backtrace.c.
 */

#include "kernel/core/backtrace.h"

__attribute__((weak)) void stack_backtrace(void) {}
