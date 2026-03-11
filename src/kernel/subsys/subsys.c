/*
 * subsys.c — Subsystem ops table registration
 *
 * The subsys_ops_table[] is indexed by pcb_t::subsys tag.
 * Each subsystem registers its ops here at compile time based on
 * CMake build flags: PPAP_ENABLE_HUMAN68K, PPAP_ENABLE_CPM.
 */

#include "subsys.h"
#include "../fs/procfs.h"
#include "../proc/proc.h"    /* SUBSYS_xxx defines */

/* Forward declarations — conditionally compiled based on CMake flags */
#ifdef PPAP_ENABLE_HUMAN68K
#include "human68k_bridge.h"
#endif

#ifdef PPAP_ENABLE_CPM
#include "cpm_bridge.h"
#endif

/* Build the ops table with enabled subsystems */
const subsys_ops_t *subsys_ops_table[SUBSYS_MAX] = {
    [0] = (const subsys_ops_t *)0,  /* SUBSYS_PPAP: default kernel behavior */
#ifdef PPAP_ENABLE_HUMAN68K
    [1] = &human68k_subsys_ops,     /* SUBSYS_HUMAN68K */
#endif
#ifdef PPAP_ENABLE_CPM
    [2] = &cpm_subsys_ops,          /* SUBSYS_CPM */
#endif
};

void subsys_init(void)
{
    procfs_register_subsys(SUBSYS_PPAP, "ppap");
#ifdef PPAP_ENABLE_HUMAN68K
    procfs_register_subsys(SUBSYS_HUMAN68K, "human68k");
#endif
#ifdef PPAP_ENABLE_ECPU_M68K
    procfs_register_ecpu("m68k");
#endif
#ifdef PPAP_ENABLE_ECPU_Z80
    procfs_register_ecpu("z80");
#endif
#ifdef PPAP_ENABLE_CPM
    procfs_register_subsys(SUBSYS_CPM, "cpm");
#endif
}
