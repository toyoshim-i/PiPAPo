/*
 * arch/riscv/.../mem_helper.c — RISC-V mm hook overrides
 *
 * Prints io_buf / DMA banner rows that the RP-style memory layout
 * reserves above the page pool.  Riscv targets share the same `#else`
 * branch of kernel/common/config.h as arm_m, so the constants are
 * identical.
 */

#include "kernel/core/mm/mem_helper.h"

#include "kernel/common/config.h"
#include "kernel/common/mod/mod_vfs.h"

void mem_helper_log_reserved(void) {
  mod_vfs.klogf("MM:   io_buf  %lx-%lx  %lu KB\n",
                (unsigned long)SRAM_IOBUF_BASE,
                (unsigned long)(SRAM_IOBUF_BASE + SRAM_IOBUF_SIZE - 1u),
                (unsigned long)(SRAM_IOBUF_SIZE / 1024u));
  mod_vfs.klogf("MM:   dma     %lx-%lx  %lu KB\n", (unsigned long)SRAM_DMA_BASE,
                (unsigned long)(SRAM_DMA_BASE + SRAM_DMA_SIZE - 1u),
                (unsigned long)(SRAM_DMA_SIZE / 1024u));
}
