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

  /* Allocate process memory */
  proc_image_segment_t region = {0};
  if (mem_region_alloc(&region, PPAP_MEM_RAM_DATA, alloc_size,
                       PROC_IMAGE_SEG_WRITABLE |
                           PROC_IMAGE_SEG_OWNED) < 0) {
    return -ENOMEM;
  }
  uint8_t *base = (uint8_t *)region.base;
#ifdef __ia16__
  {
    extern void klogf(const char *, ...);
    klogf("ELF16: base=%lx alloc=%lu entry=%lx\n",
          (unsigned long)(uintptr_t)base,
          (unsigned long)alloc_size,
          (unsigned long)ehdr->e_entry);
  }
#endif

  /* Zero entire region (covers BSS and gaps) */
  memset(base, 0, alloc_size);

  /* Second pass: copy PT_LOAD segment data */
  for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
    if (phdrs[i].p_type != PT_LOAD) continue;
    if (phdrs[i].p_filesz == 0) continue;

    /* Validate file offsets */
    if (phdrs[i].p_offset + phdrs[i].p_filesz > file_size) {
      mem_region_free(&region);
      return -ENOEXEC;
    }

    memcpy(base + phdrs[i].p_vaddr,
           file_buf + phdrs[i].p_offset,
           phdrs[i].p_filesz);
  }

  /* Track pages for process memory management */
  uint16_t npages = (uint16_t)(alloc_size / PAGE_SIZE);
  for (uint16_t i = 0; i < npages; i++) {
    if (proc_track_page(p, i, base + i * PAGE_SIZE) < 0) {
      mem_region_free(&region);
      return -ENOMEM;
    }
  }

  /* Entry point and stack setup.
   * Each user process gets CS=DS=ES = base>>4.  The ELF is linked at
   * vaddr 0, so e_entry and $msg are segment-relative offsets that
   * work directly with CS/DS set to the load segment.
   * SS stays 0 (kernel flat space) — stack operations use absolute
   * addresses.  SP points into the allocated region. */
  uint16_t proc_seg = (uint16_t)((uintptr_t)base >> 4);
  uint16_t entry_ip = (uint16_t)ehdr->e_entry;
  uint16_t sp_abs = (uint16_t)((uintptr_t)base + alloc_size);

  /* Build the initial interrupt frame on the process stack.
   * SP is an absolute address (SS=0), but CS/DS/ES use proc_seg. */
  uint16_t *frame = (uint16_t *)(uintptr_t)sp_abs;

  /* Hardware interrupt frame (popped by IRET) */
  *--frame = 0x0200;     /* FLAGS: IF=1 */
  *--frame = proc_seg;   /* CS = process segment */
  *--frame = entry_ip;   /* IP = entry point (segment-relative) */

  /* Software-saved registers (popped by ISR restore path) */
  *--frame = 0;          /* AX */
  *--frame = 0;          /* BX */
  *--frame = 0;          /* CX */
  *--frame = 0;          /* DX */
  *--frame = 0;          /* SI */
  *--frame = 0;          /* DI */
  *--frame = 0;          /* BP */
  *--frame = proc_seg;   /* DS = process segment */
  *--frame = proc_seg;   /* ES = process segment */

  p->sp = (uint32_t)(uintptr_t)frame;
#ifdef __ia16__
  {
    extern void klogf(const char *, ...);
    klogf("ELF16: sp=%lx base=%lx seg=%x\n",
          (unsigned long)(uintptr_t)frame,
          (unsigned long)(uintptr_t)base,
          (unsigned)proc_seg);
  }
#endif

  p->image.text = proc_image_segment_make(base, mem_end, PPAP_MEM_RAM_TEXT,
                                          PROC_IMAGE_SEG_EXECUTABLE);
  p->image.data = region;
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
