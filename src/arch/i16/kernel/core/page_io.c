/*
 * arch/i16/.../page_io.c — Page-payload access via segment registers
 *
 * On 8086 / V30 a normal C pointer is a 16-bit near offset into the
 * current data segment, so it cannot reach pages above the kernel's
 * 64 KB segment.  Reach the page payload through explicit far DS / ES
 * loads of the 20-bit linear address split into seg:ofs.
 *
 * rep movsb copies DS:SI → ES:DI; rep stosb fills ES:DI with AL.  The
 * SS register stays untouched; ES is restored from the saved frame.
 */

#include "kernel/core/mm/page_io.h"

#include <stdint.h>

#include "kernel/core/mm/page.h"

void page_read(page_id_t id, uint16_t off, void *buf, uint16_t len) {
  if (id == PAGE_ID_INVALID || len == 0) return;
  uint32_t linear = page_linear(id) + off;
  uint16_t seg = (uint16_t)(linear >> 4);
  uint16_t ofs = (uint16_t)(linear & 0x000Fu);
  uint16_t dst = (uint16_t)(uintptr_t)buf;
  __asm__ volatile(
      "push %%ds\n\t"
      "push %%es\n\t"
      "mov  %%ss, %%ax\n\t"
      "mov  %%ax, %%es\n\t" /* ES = SS (destination) */
      "mov  %0, %%ds\n\t"   /* DS = source segment */
      "cld\n\t"
      "rep movsb\n\t"
      "pop  %%es\n\t"
      "pop  %%ds"
      :
      : "r"(seg), "S"(ofs), "D"(dst), "c"(len)
      : "ax", "memory", "cc");
}

void page_write(page_id_t id, uint16_t off, const void *buf, uint16_t len) {
  if (id == PAGE_ID_INVALID || len == 0) return;
  uint32_t linear = page_linear(id) + off;
  uint16_t seg = (uint16_t)(linear >> 4);
  uint16_t ofs = (uint16_t)(linear & 0x000Fu);
  uint16_t src = (uint16_t)(uintptr_t)buf;
  __asm__ volatile(
      "push %%ds\n\t"
      "push %%es\n\t"
      "mov  %%ss, %%ax\n\t"
      "mov  %%ax, %%ds\n\t" /* DS = SS (source: kernel data) */
      "mov  %0, %%es\n\t"   /* ES = destination segment */
      "cld\n\t"
      "rep movsb\n\t"
      "pop  %%es\n\t"
      "pop  %%ds"
      :
      : "r"(seg), "S"(src), "D"(ofs), "c"(len)
      : "ax", "memory", "cc");
}

void page_zero(page_id_t id, uint16_t off, uint16_t len) {
  if (id == PAGE_ID_INVALID || len == 0) return;
  /* rep stosb fills ES:DI with AL.  DI and CX are declared as
   * input+output so GCC re-reads them from memory after the asm — the
   * instruction advances DI by `len` and decrements CX to zero, which
   * would desync any C-level variable GCC had mirrored to those
   * registers. */
  uint32_t linear = page_linear(id) + off;
  uint16_t seg = (uint16_t)(linear >> 4);
  uint16_t ofs = (uint16_t)(linear & 0x000Fu);
  uint16_t cx = len;
  __asm__ volatile(
      "push %%es\n\t"
      "mov  %2, %%es\n\t"
      "xor  %%al, %%al\n\t"
      "cld\n\t"
      "rep stosb\n\t"
      "pop  %%es"
      : "+D"(ofs), "+c"(cx)
      : "r"(seg)
      : "ax", "memory", "cc");
}
