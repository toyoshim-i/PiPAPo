/*
 * arch/arm_m/.../mem_helper.c — ARM Cortex-M mm hook overrides
 *
 * The ARM page pool is statically located, so no init_pool override is
 * needed.  This file only carries the post-init hook that runs the
 * XIP verification benchmark under PPAP_TESTS — it touches XIP-only
 * symbols (xip_add / xip_bench) that exist exclusively in ARM builds.
 */

#include "kernel/core/mm/mem_helper.h"

#if defined(PPAP_TESTS)
#include "kernel/core/xip.h"
#endif

void mem_helper_post_init(void) {
#if defined(PPAP_TESTS)
  xip_verify();
#endif
}
