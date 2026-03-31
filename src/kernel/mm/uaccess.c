/*
 * uaccess.c — User-space memory access implementation
 *
 * On 32-bit flat targets (ARM, m68k, RISC-V), user_addr is a linear
 * address directly dereferenceable by the kernel.  Access is a plain
 * memcpy with a bounds check against the process's tracked pages.
 *
 * On i16 (8086 real mode), user_addr is a 20-bit linear address
 * (user_ds * 16 + offset).  The kernel cannot dereference it with a
 * 16-bit pointer, so we resolve it to page_id + offset and use
 * mm_page_read/mm_page_write (which handle segment register setup).
 *
 * On Xtensa, user_addr may point to IRAM (word-access only) or PSRAM,
 * but the kernel can dereference it directly.  Same as 32-bit path.
 */

#include "uaccess.h"

#include <string.h>

#include "kernel/common/errno.h"
#include "kernel/proc/proc.h"
#include "mem_region.h"
#include "page.h"


/* ── 32-bit implementation ───────────────────────────────────────────────── */

/*
 * On 32-bit flat targets without an MMU (ARM Cortex-M, m68k, RISC-V),
 * the kernel can dereference any address in the process's address space.
 * No segment resolution is needed — a plain memcpy suffices.
 *
 * access_ok() is not called here because subsystem bridges and kernel
 * tests legitimately call sys_read/sys_write with kernel-space buffers.
 * On a flat no-MMU target there is no meaningful user/kernel address
 * boundary to enforce.
 */

#if !defined(__ia16__)

int copy_from_user(void *kbuf, uint32_t user_addr, size_t n) {
  if (n == 0) return 0;
  memcpy(kbuf, (const void *)(uintptr_t)user_addr, n);
  return 0;
}

int copy_to_user(uint32_t user_addr, const void *kbuf, size_t n) {
  if (n == 0) return 0;
  memcpy((void *)(uintptr_t)user_addr, kbuf, n);
  return 0;
}

int strncpy_from_user(char *kbuf, uint32_t user_addr, size_t max) {
  if (max == 0) return -EFAULT;
  const char *src = (const char *)(uintptr_t)user_addr;
  size_t i;
  for (i = 0; i < max - 1; i++) {
    kbuf[i] = src[i];
    if (kbuf[i] == '\0') return (int)i;
  }
  kbuf[i] = '\0';
  return (int)i;
}

/* ── i16 implementation ──────────────────────────────────────────────────── */

#else /* __ia16__ */

/*
 * On i16, user_addr is a 20-bit linear address.  We resolve it to a
 * page index and use mm_page_read/mm_page_write which handle the
 * segment register gymnastics.
 *
 * Copies that span page boundaries are split into per-page chunks.
 */

/* Find the page_id for a linear address within current's tracked pages. */
static page_id_t find_user_page(uint32_t linear) {
  const pcb_t *p = current;
  for (uint32_t i = 0; i < USER_PAGES_MAX; i++) {
    if (p->user_pages[i] == PAGE_ID_INVALID) continue;
    uint32_t base = mem_region_page_linear(p->user_pages[i]);
    if (linear >= base && linear < base + PAGE_SIZE)
      return p->user_pages[i];
  }
  /* Check kernel stack page. */
  if (p->stack_page_id != PAGE_ID_INVALID) {
    uint32_t sbase = mem_region_page_linear(p->stack_page_id);
    if (linear >= sbase && linear < sbase + PAGE_SIZE)
      return p->stack_page_id;
  }
  return PAGE_ID_INVALID;
}

int copy_from_user(void *kbuf, uint32_t user_addr, size_t n) {
  uint8_t *dst = (uint8_t *)kbuf;
  uint32_t addr = user_addr;
  size_t remaining = n;

  while (remaining > 0) {
    page_id_t pid = find_user_page(addr);
    if (pid == PAGE_ID_INVALID) return -EFAULT;

    uint32_t page_base = mem_region_page_linear(pid);
    uint16_t off = (uint16_t)(addr - page_base);
    uint16_t chunk = PAGE_SIZE - off;
    if (chunk > remaining) chunk = (uint16_t)remaining;

    mem_region_page_read(pid, off, dst, chunk);

    dst += chunk;
    addr += chunk;
    remaining -= chunk;
  }
  return 0;
}

int copy_to_user(uint32_t user_addr, const void *kbuf, size_t n) {
  const uint8_t *src = (const uint8_t *)kbuf;
  uint32_t addr = user_addr;
  size_t remaining = n;

  while (remaining > 0) {
    page_id_t pid = find_user_page(addr);
    if (pid == PAGE_ID_INVALID) return -EFAULT;

    uint32_t page_base = mem_region_page_linear(pid);
    uint16_t off = (uint16_t)(addr - page_base);
    uint16_t chunk = PAGE_SIZE - off;
    if (chunk > remaining) chunk = (uint16_t)remaining;

    mem_region_page_write(pid, off, src, chunk);

    src += chunk;
    addr += chunk;
    remaining -= chunk;
  }
  return 0;
}

int strncpy_from_user(char *kbuf, uint32_t user_addr, size_t max) {
  if (max == 0) return -EFAULT;

  size_t i = 0;
  uint32_t addr = user_addr;

  while (i < max - 1) {
    page_id_t pid = find_user_page(addr);
    if (pid == PAGE_ID_INVALID) {
      kbuf[0] = '\0';
      return -EFAULT;
    }

    uint32_t page_base = mem_region_page_linear(pid);
    uint16_t off = (uint16_t)(addr - page_base);
    uint16_t avail = PAGE_SIZE - off;
    uint16_t chunk = (max - 1 - i < avail) ? (uint16_t)(max - 1 - i) : avail;

    /* Read a chunk and scan for NUL. */
    mem_region_page_read(pid, off, kbuf + i, chunk);
    for (uint16_t j = 0; j < chunk; j++) {
      if (kbuf[i + j] == '\0') return (int)(i + j);
    }

    i += chunk;
    addr += chunk;
  }
  kbuf[i] = '\0';
  return (int)i;
}

#endif /* __ia16__ */
