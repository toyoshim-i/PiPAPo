/*
 * elf16_loader.c — 16-bit ELF loader for i8086 real mode
 *
 * Loads static ELF32 executables produced by ia16-elf-gcc into a
 * contiguous memory region.  Each process gets its own segment
 * (CS=DS=SS=base>>4).
 *
 * Limitations:
 *   - Static executables only (ET_EXEC, no relocations)
 *   - All PT_LOAD segments must fit in 64 KB
 *   - Linked at vaddr 0x0000 (standard for ia16-elf-ld)
 *   - No shared libraries, no PIE
 */

#include "loader.h"
#include "elf.h"

#include <string.h>

#include "kernel/common/errno.h"
#include "kernel/mm/mem_region.h"
#include "kernel/mm/page.h"
#include "kernel/proc/proc.h"
#include "arch/arch.h"
#include "kernel/cpu/cpu.h"

#define ELF16_MAX_SIZE  (60u * 1024u)  /* 60 KB max (leave room for stack) */
#define ELF16_STACK_SIZE 2048u         /* 2 KB user stack */

/* ── Detection ─────────────────────────────────────────────────────────── */

static int elf16_detect(const uint8_t *buf, uint32_t size, const char *path)
{
  (void)path;

  /* Only on native i16 host */
  if (HOST_ARCH_ID != CPU_ARCH_8086) return 0;

  /* Need at least the ELF header */
  if (size < sizeof(elf32_ehdr_t)) return 0;

  const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)buf;

  /* Check ELF magic */
  if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
      ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
      ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
      ehdr->e_ident[EI_MAG3] != ELFMAG3)
    return 0;

  /* Must be 32-bit, little-endian, executable, EM_386 */
  if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) return 0;
  if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) return 0;
  if (ehdr->e_type != ET_EXEC) return 0;
  if (ehdr->e_machine != EM_386) return 0;

  return 1;
}

/* ── Loading ───────────────────────────────────────────────────────────── */

static int elf16_load(pcb_t *p, const uint8_t *file_buf, uint32_t file_size,
                      const cpu_ops_t *cpu_ops, void *cpu_state,
                      const char *const *argv, uint32_t flags)
{
  (void)flags;
  (void)cpu_ops;
  (void)cpu_state;
  (void)argv;

  const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)file_buf;

  /* Validate program header table */
  if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return -ENOEXEC;
  if (ehdr->e_phoff + ehdr->e_phnum * sizeof(elf32_phdr_t) > file_size)
    return -ENOEXEC;

  const elf32_phdr_t *phdrs = (const elf32_phdr_t *)(file_buf + ehdr->e_phoff);

  /* First pass: find total memory footprint of PT_LOAD segments */
  uint32_t mem_end = 0;
  for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
    if (phdrs[i].p_type != PT_LOAD) continue;
    uint32_t seg_end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
    if (seg_end > mem_end) mem_end = seg_end;
  }

  if (mem_end == 0) return -ENOEXEC;
  if (mem_end > ELF16_MAX_SIZE) return -ENOMEM;

  /* Round up to page boundary, add stack space */
  uint32_t alloc_size = ((mem_end + ELF16_STACK_SIZE + PAGE_SIZE - 1)
                         / PAGE_SIZE) * PAGE_SIZE;
  uint16_t npages = (uint16_t)(alloc_size / PAGE_SIZE);

  /* Allocate contiguous pages via page-indexed API.
   * mm_page_alloc_contiguous returns a page_id_t (index), not a
   * pointer — safe on i16 where near pointers can't address pages
   * above 64 KB.  mm_page_write copies data via segment:offset. */
  if (npages > 16) return -ENOMEM;
  page_id_t base_id = mm_page_alloc_contiguous(npages);
  if (base_id == PAGE_ID_INVALID) return -ENOMEM;

  /* Zero entire region via page-indexed writes */
  {
    uint8_t zeros[256];
    memset(zeros, 0, sizeof(zeros));
    for (uint16_t pg = 0; pg < npages; pg++) {
      for (uint16_t off = 0; off < PAGE_SIZE; off += sizeof(zeros))
        mm_page_write(base_id + pg, off, zeros, sizeof(zeros));
    }
  }

  /* Second pass: copy PT_LOAD segment data via mm_page_write */
  for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
    if (phdrs[i].p_type != PT_LOAD) continue;
    if (phdrs[i].p_filesz == 0) continue;

    if (phdrs[i].p_offset + phdrs[i].p_filesz > file_size) {
      for (uint16_t j = 0; j < npages; j++)
        mm_page_free(base_id + j);
      return -ENOEXEC;
    }

    uint32_t vaddr = phdrs[i].p_vaddr;
    uint32_t remaining = phdrs[i].p_filesz;
    const uint8_t *src = file_buf + phdrs[i].p_offset;

    while (remaining > 0) {
      uint16_t pg_idx = (uint16_t)(vaddr / PAGE_SIZE);
      uint16_t pg_off = (uint16_t)(vaddr % PAGE_SIZE);
      uint16_t chunk = PAGE_SIZE - pg_off;
      if (chunk > remaining) chunk = (uint16_t)remaining;

      mm_page_write(base_id + pg_idx, pg_off, src, chunk);
      src += chunk;
      vaddr += chunk;
      remaining -= chunk;
    }
  }

  /* Compute process segment from the 32-bit linear address of page 0. */
  uint32_t base_linear = mm_page_linear(base_id);
  uint16_t proc_seg = (uint16_t)(base_linear >> 4);
  uint16_t entry_ip = (uint16_t)ehdr->e_entry;

  /* Stack: SP is an absolute linear address (SS=0).
   * Stack top = base_linear + alloc_size. */
  uint32_t sp_linear = base_linear + alloc_size;

  /* Build the initial interrupt frame on the process stack.
   * Use mm_page_write to place it (avoids near-pointer truncation).
   * Frame grows downward: HW frame (6B) on top, then SW frame (18B). */
  uint16_t hw_frame[3];
  hw_frame[0] = entry_ip;     /* IP */
  hw_frame[1] = proc_seg;     /* CS */
  hw_frame[2] = 0x0200;       /* FLAGS: IF=1 */

  uint16_t sw_frame[9];
  sw_frame[0] = proc_seg;     /* ES = process segment */
  sw_frame[1] = proc_seg;     /* DS = process segment */
  sw_frame[2] = 0;            /* BP */
  sw_frame[3] = 0;            /* DI */
  sw_frame[4] = 0;            /* SI */
  sw_frame[5] = 0;            /* DX */
  sw_frame[6] = 0;            /* CX */
  sw_frame[7] = 0;            /* BX */
  sw_frame[8] = 0;            /* AX */

  uint32_t hw_pos = sp_linear - sizeof(hw_frame);
  uint16_t hw_pg = (uint16_t)((hw_pos - base_linear) / PAGE_SIZE);
  uint16_t hw_off = (uint16_t)((hw_pos - base_linear) % PAGE_SIZE);
  mm_page_write(base_id + hw_pg, hw_off, hw_frame, sizeof(hw_frame));

  uint32_t sw_pos = hw_pos - sizeof(sw_frame);
  uint16_t sw_pg = (uint16_t)((sw_pos - base_linear) / PAGE_SIZE);
  uint16_t sw_off = (uint16_t)((sw_pos - base_linear) % PAGE_SIZE);
  mm_page_write(base_id + sw_pg, sw_off, sw_frame, sizeof(sw_frame));

  p->sp = (uint32_t)sw_pos;

  /* Track pages by index — no pointer truncation. */
  for (uint16_t i = 0; i < npages; i++)
    proc_track_page(p, i, base_id + i);

  p->image.text = proc_image_segment_make(
      (void *)(uintptr_t)(uint16_t)base_linear, mem_end,
      PPAP_MEM_RAM_TEXT, PROC_IMAGE_SEG_EXECUTABLE);
  /* Data region: freed via proc_release_tracked_pages, not OWNED. */
  p->image.data = proc_image_segment_make(
      (void *)(uintptr_t)(uint16_t)base_linear, alloc_size,
      PPAP_MEM_RAM_DATA, PROC_IMAGE_SEG_WRITABLE);
  p->image.entry = (uintptr_t)entry_ip;
  p->ticks_remaining = PROC_DEFAULT_TICKS;

  return 0;
}

/* ── Loader registration ──────────────────────────────────────────────── */

const loader_t elf16_loader = {
    .name = "elf16",
    .detect = elf16_detect,
    .load = elf16_load,
    .required_arch_id = CPU_ARCH_8086,
    .xip = 0,
};
