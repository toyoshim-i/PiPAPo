/*
 * x68k_iocs.c — X68000 IOCS process-context serialization
 */

#include "kernel/vfs/driver/x68k_iocs.h"

#include "kernel/common/core/proc_info.h"
#include "kernel/common/mod/mod_core.h"
#include "kernel/common/sync/kmutex.h"

#define MFP_IMRB_ADDR 0xE88015u
#define MFP_TC_BIT 0x20u
#define IOCS_IRQ_SR_SHIFT 16u
#define IOCS_IRQ_IMRB_MASK 0xFFu

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

int x68k_iocs_try_enter(void) {
  if (!current) return 0;
  if (!iocs_ready) x68k_iocs_init();
  return mod_core.kmutex_try_lock(&iocs_mutex);
}

x68k_iocs_irq_state_t x68k_iocs_irq_begin(void) {
  volatile uint8_t *imrb = (volatile uint8_t *)MFP_IMRB_ADDR;
  uint8_t saved_imrb = *imrb;
  *imrb = (uint8_t)(saved_imrb & (uint8_t)~MFP_TC_BIT);

  uint16_t saved_sr;
  asm volatile(
      "move.w %%sr,%0\n\t"
      "andi.w #0xF8FF,%%sr"
      : "=d"(saved_sr)
      :
      : "memory");

  return ((x68k_iocs_irq_state_t)saved_sr << IOCS_IRQ_SR_SHIFT) |
         saved_imrb;
}

void x68k_iocs_irq_end(x68k_iocs_irq_state_t state) {
  volatile uint8_t *imrb = (volatile uint8_t *)MFP_IMRB_ADDR;
  uint16_t saved_sr = (uint16_t)(state >> IOCS_IRQ_SR_SHIFT);
  uint8_t saved_imrb = (uint8_t)(state & IOCS_IRQ_IMRB_MASK);

  asm volatile("move.w %0,%%sr" : : "d"(saved_sr) : "memory");
  *imrb = saved_imrb;
}
