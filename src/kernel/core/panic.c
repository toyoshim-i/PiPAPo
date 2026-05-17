/*
 * panic.c — Kernel panic implementation
 */

#include "kernel/core/panic.h"

#include <stdarg.h>

#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/arch.h"
#include "target/target.h"

void panic(const char *fmt, ...) {
  mod_vfs.klogf("PANIC: ");
  va_list ap;
  va_start(ap, fmt);
  mod_vfs.kvlogf(fmt, ap);
  va_end(ap);
  target_may_poweroff(1);
  for (;;) arch_wfi();
}
