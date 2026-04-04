/*
 * subsys.c — Subsystem ops table registration
 *
 * The subsys_ops_table[] is indexed by pcb_t::subsys tag.
 * Each subsystem registers its ops here at compile time based on
 * CMake build flags: PPAP_ENABLE_HUMAN68K, PPAP_ENABLE_CPM, PPAP_ENABLE_SOS.
 */

#include "subsys.h"

#include "common/core/subsys_info.h"
#include "common/core/ecpu_info.h"

/* Forward declarations — conditionally compiled based on CMake flags */
#ifdef PPAP_ENABLE_HUMAN68K
#include "human68k_bridge.h"
#endif

#ifdef PPAP_ENABLE_CPM
#include "cpm_bridge.h"
#endif

#ifdef PPAP_ENABLE_SOS
#include "sos_bridge.h"
#endif

/* Statically initialised name arrays — read by procfs via shared headers */
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

void subsys_init(void) {
  /* Name arrays are statically initialised above. */
}
