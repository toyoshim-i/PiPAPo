/*
 * subsys.c — Subsystem ops table registration
 *
 * The subsys_ops_table[] is indexed by pcb_t::subsys tag.
 * Each subsystem registers its ops here at compile time based on
 * CMake build flags: PPAP_ENABLE_HUMAN68K, PPAP_ENABLE_CPM, PPAP_ENABLE_SOS.
 */

#include "kernel/core/subsys/subsys.h"

#include "kernel/common/core/ecpu_info.h"
#include "kernel/common/core/subsys_info.h"

/* Forward declarations — conditionally compiled based on CMake flags */
#ifdef PPAP_ENABLE_HUMAN68K
#include "kernel/core/subsys/human68k_bridge.h"
#endif

#ifdef PPAP_ENABLE_CPM
#include "kernel/core/subsys/cpm_bridge.h"
#endif

#ifdef PPAP_ENABLE_SOS
#include "kernel/core/subsys/sos_bridge.h"
#endif

/* Statically initialised name arrays — shared via subsys_info.h */
const char *subsys_names[SUBSYS_MAX] = {
    [SUBSYS_PPAP] = "ppap",
#ifdef PPAP_ENABLE_HUMAN68K
    [SUBSYS_HUMAN68K] = "human68k",
#endif
#ifdef PPAP_ENABLE_CPM
    [SUBSYS_CPM] = "cpm",
#endif
#ifdef PPAP_ENABLE_SOS
    [SUBSYS_SOS] = "sos",
#endif
};

const char *ecpu_names[ECPU_MAX] = {
#ifdef PPAP_ENABLE_ECPU_M68K
    "m68k",
#endif
#ifdef PPAP_ENABLE_ECPU_Z80
    "z80",
#endif
};

/* Build the ops table with enabled subsystems */
const subsys_ops_t *subsys_ops_table[SUBSYS_MAX] = {
    [SUBSYS_PPAP] = (const subsys_ops_t *)0,
#ifdef PPAP_ENABLE_HUMAN68K
    [SUBSYS_HUMAN68K] = &human68k_subsys_ops,
#endif
#ifdef PPAP_ENABLE_CPM
    [SUBSYS_CPM] = &cpm_subsys_ops,
#endif
#ifdef PPAP_ENABLE_SOS
    [SUBSYS_SOS] = &sos_subsys_ops,
#endif
};

void subsys_init(void) { /* Name arrays are statically initialised above. */ }

/* ── Query interface (called by procfs via subsys_info.h) ────────────── */

int subsys_read_proc(int tag, struct pcb *p, const char *name, char *buf,
                     int bufsiz) {
  if ((unsigned)tag >= SUBSYS_MAX) return -1;
  if (!subsys_ops_table[tag]) return -1;
  if (!subsys_ops_table[tag]->on_proc_read) return -1;
  if (!buf) return 0; /* probe: handler exists */
  return subsys_ops_table[tag]->on_proc_read(p, name, buf, bufsiz);
}
