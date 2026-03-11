/*
 * subsys.c — Subsystem ops table registration
 *
 * The subsys_ops_table[] is indexed by pcb_t::subsys tag.
 * Each subsystem registers its ops here at compile time.
 */

#include "subsys.h"
#include "../fs/procfs.h"
#include "../proc/proc.h"    /* SUBSYS_xxx defines */

/* Forward declaration — only include on m68k where Human68k is relevant */
#if defined(__m68k__)
#include "human68k_bridge.h"
#endif

const subsys_ops_t *subsys_ops_table[SUBSYS_MAX] = {
    [0] = (const subsys_ops_t *)0,  /* SUBSYS_PPAP: default kernel behavior */
#if defined(__m68k__)
    [1] = &human68k_subsys_ops,     /* SUBSYS_HUMAN68K */
#endif
};

void subsys_init(void)
{
    procfs_register_subsys(SUBSYS_PPAP, "ppap");
#if defined(__m68k__)
    procfs_register_subsys(SUBSYS_HUMAN68K, "human68k");
#endif
}
