/*
 * x68k_iocs.c — X68000 IOCS process-context serialization
 */

#include "kernel/vfs/driver/x68k_iocs.h"

#include "kernel/common/core/proc_info.h"
#include "kernel/common/mod/mod_core.h"
#include "kernel/common/sync/kmutex.h"

static kmutex_t iocs_mutex;
static int iocs_ready;

void x68k_iocs_init(void) {
  if (iocs_ready) return;
  mod_core.kmutex_init(&iocs_mutex);
  iocs_ready = 1;
}

void x68k_iocs_enter(void) {
  if (!current) return;
  if (!iocs_ready) x68k_iocs_init();
  mod_core.kmutex_lock(&iocs_mutex);
}

void x68k_iocs_exit(void) {
  if (!current) return;
  mod_core.kmutex_unlock(&iocs_mutex);
}

int x68k_iocs_held_by_current(void) {
  return current && iocs_mutex.owner == current;
}
