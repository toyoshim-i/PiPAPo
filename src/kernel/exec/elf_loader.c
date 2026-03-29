/*
 * elf_loader.c — ELF binary format loader
 */

#include "elf_loader.h"

#include <string.h>

#include "arch/arch.h"
#include "elf.h"
#include "kernel/common/errno.h"
#include "kernel/klog.h"
#include "kernel/mm/mem_region.h"
#include "kernel/mm/page.h"
#include "kernel/proc/proc.h"


static int elf_detect(const uint8_t *file_buf, uint32_t file_size,
                      const char *path) {
  (void)path;
  if (file_size < sizeof(elf32_ehdr_t)) return 0;
  const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)file_buf;
  return elf_validate(ehdr) == 0;
}

#define MAX_LOAD_SEGS 4

typedef struct {
  uint32_t entry;
  uint32_t stack_top;
  uint32_t got_sram_addr;
  uint32_t load_base; /* effective base for gp (= data_base - data_va) */
} elf_load_result_t;

typedef enum {
  ELF_TEXT_XIP,   /* Execute text in-place from file buffer */
  ELF_TEXT_SRAM,  /* Copy text to SRAM (PPAP_MEM_RAM_TEXT) */
} elf_text_mode_t;

/* Decide whether text can execute in-place or must be copied to SRAM.
 * Xtensa always copies (IRAM/PSRAM is not byte-addressable from flash).
 * Other arches XIP when the source supports it (e.g. romfs). */
static elf_text_mode_t elf_text_mode(int source_is_xip_capable) {
#if defined(__xtensa__)
  /* Xtensa: IRAM/PSRAM requires copy (not flash-XIP capable). */
  (void)source_is_xip_capable;
  return ELF_TEXT_SRAM;
#else
  return source_is_xip_capable ? ELF_TEXT_XIP : ELF_TEXT_SRAM;
#endif
}

/* --- Relocation context and per-arch callbacks --- */

typedef struct {
  const elf32_ehdr_t *ehdr;
  const uint8_t *file_buf;
  uint32_t file_size;
  uint32_t text_base;
  uint8_t *data_base;
  uint32_t text_end_va;
  uint32_t data_va;
  uint32_t data_memsz;
  const elf32_phdr_t *text_seg;
  const elf32_phdr_t *data_seg;
  const cpu_ops_t *cpu_ops;
  void *cpu_state;
} elf_reloc_ctx_t;

static inline uint32_t elf_split_addr(uint32_t link_addr, uint32_t text_base,
                                      uint32_t data_base, uint32_t data_va) {
  if (link_addr < data_va)
    return link_addr + text_base;
  return data_base + (link_addr - data_va);
}

static __attribute__((unused))
void elf_reloc_got_split(const elf_reloc_ctx_t *ctx) {
  const elf32_ehdr_t *ehdr = ctx->ehdr;
  uint32_t dram_base = ctx->data_base ? (uint32_t)(uintptr_t)ctx->data_base : 0;
  uint32_t image_end = ctx->data_va + ctx->data_memsz;
  if (!ctx->data_base || !ehdr->e_shoff || !ehdr->e_shnum ||
      ehdr->e_shentsize < 40)
    return;
  for (uint16_t si = 0; si < ehdr->e_shnum; si++) {
    const uint8_t *sh =
        ctx->file_buf + ehdr->e_shoff + si * ehdr->e_shentsize;
    uint32_t sh_type    = *(const uint32_t *)(sh + 4);
    uint32_t sh_flags   = *(const uint32_t *)(sh + 8);
    uint32_t sh_addr    = *(const uint32_t *)(sh + 12);
    uint32_t sh_size    = *(const uint32_t *)(sh + 20);
    uint32_t sh_entsize = *(const uint32_t *)(sh + 36);
    if (sh_type != 1 /* SHT_PROGBITS */) continue;
    if (!(sh_flags & 3 /* SHF_WRITE|SHF_ALLOC */)) continue;
    if (sh_entsize != 4) continue;
    if (sh_addr >= ctx->data_va && sh_size > 0) {
      uint32_t n_got = sh_size / 4;
      uint32_t *got =
          (uint32_t *)(ctx->data_base + (sh_addr - ctx->data_va));
      for (uint32_t gi = 0; gi < n_got; gi++) {
        uint32_t val = got[gi];
        if (val == 0 || val == 0xFFFFFFFFu) continue;
        if (val < image_end)
          got[gi] = elf_split_addr(val, ctx->text_base, dram_base,
                                   ctx->data_va);
      }
    }
  }
}

#if defined(__xtensa__)
static int elf_reloc_arch(const elf_reloc_ctx_t *ctx, elf_load_result_t *out) {
  const elf32_ehdr_t *ehdr = ctx->ehdr;
  uint32_t dram_base = ctx->data_base ? (uint32_t)(uintptr_t)ctx->data_base : 0;
  (void)out;
  if (ehdr->e_shoff && ehdr->e_shnum && ehdr->e_shentsize >= 40) {
    for (uint16_t si = 0; si < ehdr->e_shnum; si++) {
      const uint8_t *sh =
          ctx->file_buf + ehdr->e_shoff + si * ehdr->e_shentsize;
      uint32_t sh_type = *(const uint32_t *)(sh + 4);
      if (sh_type != 4 /* SHT_RELA */) continue;
      uint32_t sh_info_idx = *(const uint32_t *)(sh + 28);
      if (sh_info_idx >= ehdr->e_shnum) continue;
      const uint8_t *target_sh =
          ctx->file_buf + ehdr->e_shoff + sh_info_idx * ehdr->e_shentsize;
      uint32_t target_flags = *(const uint32_t *)(target_sh + 8);
      if (!(target_flags & 2 /* SHF_ALLOC */)) continue;
      uint32_t sh_offset  = *(const uint32_t *)(sh + 16);
      uint32_t sh_size    = *(const uint32_t *)(sh + 20);
      uint32_t sh_entsize = *(const uint32_t *)(sh + 36);
      if (sh_entsize < 12) continue;
      uint32_t n_entries = sh_size / sh_entsize;
      for (uint32_t ri = 0; ri < n_entries; ri++) {
        const uint8_t *rela = ctx->file_buf + sh_offset + ri * sh_entsize;
        uint32_t r_offset = *(const uint32_t *)(rela + 0);
        uint32_t r_info   = *(const uint32_t *)(rela + 4);
        uint8_t  r_type   = (uint8_t)(r_info & 0xFF);
        if (r_type != 1 && r_type != 6) continue;
        volatile uint32_t *target;
        if (r_offset < ctx->text_end_va)
          target = (volatile uint32_t *)((uintptr_t)ctx->text_base + r_offset);
        else if (ctx->data_base && r_offset >= ctx->data_va)
          target = (volatile uint32_t *)(ctx->data_base +
                                         (r_offset - ctx->data_va));
        else
          continue;
        uint32_t val = *target;
        *target = elf_split_addr(val, ctx->text_base, dram_base, ctx->data_va);
      }
    }
  }
  elf_reloc_got_split(ctx);
  return 0;
}
#elif defined(__riscv)
/* Resolve a link-time address to a runtime address for RISC-V.
 *
 * With the ePIC linker script, the layout is:
 *   text (R+X, XIP) | rodata (R, SRAM) | data (RW, SRAM)
 * Rodata and data are contiguous in SRAM starting at data_va.
 * Text is XIP from the file buffer. */
static uint32_t riscv_resolve_addr(const elf_reloc_ctx_t *ctx,
                                   uint32_t link_addr) {
  uint32_t dram_base = ctx->data_base ? (uint32_t)(uintptr_t)ctx->data_base : 0;
  /* Rodata + data: everything at or above data_va is in SRAM */
  if (link_addr >= ctx->data_va)
    return dram_base + (link_addr - ctx->data_va);
  /* Text segment (XIP) */
  if (ctx->text_seg)
    return ctx->text_base + (link_addr - ctx->text_seg->p_vaddr);
  return link_addr;
}

static int elf_reloc_arch(const elf_reloc_ctx_t *ctx, elf_load_result_t *out) {
  const elf32_ehdr_t *ehdr = ctx->ehdr;
  (void)out;
  if (ehdr->e_shoff && ehdr->e_shnum && ehdr->e_shentsize >= 40) {
    for (uint16_t si = 0; si < ehdr->e_shnum; si++) {
      const uint8_t *sh =
          ctx->file_buf + ehdr->e_shoff + si * ehdr->e_shentsize;
      uint32_t sh_type = *(const uint32_t *)(sh + 4);
      if (sh_type != 4 /* SHT_RELA */) continue;
      uint32_t sh_offset = *(const uint32_t *)(sh + 16);
      uint32_t sh_size   = *(const uint32_t *)(sh + 20);
      uint32_t sh_entsize = *(const uint32_t *)(sh + 36);
      if (sh_entsize < 12) continue;
      uint32_t n_entries = sh_size / sh_entsize;
      for (uint32_t ri = 0; ri < n_entries; ri++) {
        const uint8_t *rela = ctx->file_buf + sh_offset + ri * sh_entsize;
        uint32_t r_offset = *(const uint32_t *)(rela + 0);
        uint32_t r_info   = *(const uint32_t *)(rela + 4);
        uint8_t  r_type   = (uint8_t)(r_info & 0xFF);
        if (r_type == 0 || r_type == 16 || r_type == 17 || r_type == 20 ||
            r_type == 23 || r_type == 24 || r_type == 25 || r_type == 35 ||
            r_type == 39 || r_type == 44 || r_type == 45 || r_type == 51 ||
            (r_type >= 53 && r_type <= 72))
          continue;
        if (r_type == 3 /* R_RISCV_RELATIVE */) {
          int32_t r_addend = *(const int32_t *)(rela + 8);
          uint32_t *target;
          if (ctx->data_base && r_offset >= ctx->data_va &&
              r_offset < ctx->data_va + ctx->data_memsz)
            target = (uint32_t *)(ctx->data_base + (r_offset - ctx->data_va));
          else
            continue;
          *target = riscv_resolve_addr(ctx, (uint32_t)r_addend);
          continue;
        }
        klogf("ELF: unhandled RISCV reloc type %lu at offset %lx\n",
              (unsigned long)r_type, (unsigned long)r_offset);
        return -(int)ENOEXEC;
      }
    }
  }
  elf_reloc_got_split(ctx);
  return 0;
}
#else /* ARM/m68k */
static int apply_relocations(const elf32_ehdr_t *ehdr,
                              const uint8_t *file_base, uint32_t file_size,
                              const elf32_phdr_t *text_seg,
                              const elf32_phdr_t *data_seg, uint8_t *sram_page,
                              uint32_t text_base, uint32_t got_sram_addr,
                              const elf_got_info_t *got_info,
                              const cpu_ops_t *cpu_ops, void *cpu_state) {
  elf_rel_info_t rel_info;
  if (elf_find_rel(ehdr, file_base, &rel_info, file_size) != 0) return 0;
  const uint8_t *rel_base = file_base + rel_info.offset;
  uint32_t entry_size =
      rel_info.has_addend ? sizeof(elf32_rela_t) : sizeof(elf32_rel_t);
  uint32_t n_rel = rel_info.size / entry_size;
  elf_dynsym_info_t dynsym_info = {0, 0};
  const elf32_sym_t *dynsym = NULL;
  if (elf_find_dynsym(ehdr, file_base, &dynsym_info, file_size) == 0)
    dynsym = (const elf32_sym_t *)(file_base + dynsym_info.offset);
  for (uint32_t i = 0; i < n_rel; i++) {
    uint32_t r_offset, r_info;
    int32_t r_addend = 0;
    if (rel_info.has_addend) {
      const elf32_rela_t *rela =
          (const elf32_rela_t *)(rel_base + i * entry_size);
      r_offset = rela->r_offset;
      r_info = rela->r_info;
      r_addend = rela->r_addend;
    } else {
      const elf32_rel_t *rel = (const elf32_rel_t *)(rel_base + i * entry_size);
      r_offset = rel->r_offset;
      r_info = rel->r_info;
    }
    uint32_t rtype = ELF32_R_TYPE(r_info);
    if (rtype == 0) continue; /* R_*_NONE — padding, skip */
    if ((cpu_ops->arch_id == CPU_ARCH_M68K && rtype == R_68K_JMP_SLOT) ||
        (cpu_ops->arch_id == CPU_ARCH_ARM && rtype == R_ARM_JMP_SLOT)) {
      if (!dynsym) continue;
      uint32_t sym_idx = ELF32_R_SYM(r_info);
      if (sym_idx >= dynsym_info.count) continue;
      uint32_t off = r_offset;
      if (off < data_seg->p_vaddr ||
          off >= data_seg->p_vaddr + data_seg->p_memsz)
        continue;
      uint32_t off_in_sram = off - data_seg->p_vaddr;
      uint32_t word_addr = (uint32_t)(uintptr_t)sram_page + off_in_sram;
      uint32_t sym_val = dynsym[sym_idx].st_value + (uint32_t)r_addend;
      if (sym_val < data_seg->p_vaddr)
        cpu_ops->write32(cpu_state, word_addr, sym_val + text_base);
      else
        cpu_ops->write32(cpu_state, word_addr,
            sym_val - data_seg->p_vaddr + (uint32_t)(uintptr_t)sram_page);
      continue;
    }
    if ((cpu_ops->arch_id == CPU_ARCH_M68K && rtype != R_68K_RELATIVE) ||
        (cpu_ops->arch_id == CPU_ARCH_ARM && rtype != R_ARM_RELATIVE) ||
        (cpu_ops->arch_id == CPU_ARCH_ARMV6 && rtype != R_ARM_RELATIVE)) {
      klogf("ELF: unhandled reloc type %lu at offset %lx\n", (unsigned long)rtype, (unsigned long)r_offset);
      return -1;
    }
    uint32_t off = r_offset;
    if (off < data_seg->p_vaddr || off >= data_seg->p_vaddr + data_seg->p_memsz)
      continue;
    if (got_sram_addr != 0 && got_info && off >= got_info->addr &&
        off < got_info->addr + got_info->size)
      continue;
    uint32_t off_in_sram = off - data_seg->p_vaddr;
    uint32_t word_addr = (uint32_t)(uintptr_t)sram_page + off_in_sram;
    uint32_t word_val = cpu_ops->read32(cpu_state, word_addr);
    uint32_t val = rel_info.has_addend ? (uint32_t)r_addend : word_val;
    if (val == 0) continue;
    if (val < data_seg->p_vaddr)
      cpu_ops->write32(cpu_state, word_addr, val + text_base);
    else
      cpu_ops->write32(cpu_state, word_addr,
          val - data_seg->p_vaddr + (uint32_t)(uintptr_t)sram_page);
  }
  return 0;
}

static int elf_reloc_arch(const elf_reloc_ctx_t *ctx, elf_load_result_t *out) {
  elf_got_info_t got_info = {0, 0, 0};
  if (ctx->data_base &&
      elf_find_got(ctx->ehdr, ctx->file_buf, &got_info, ctx->file_size) == 0) {
    uint32_t got_offset_in_data = got_info.addr - ctx->data_seg->p_vaddr;
    out->got_sram_addr =
        (uint32_t)(uintptr_t)ctx->data_base + got_offset_in_data;
    uint32_t n_entries = got_info.size / 4;
    for (uint32_t i = 0; i < n_entries; i++) {
      uint32_t word_addr = out->got_sram_addr + (i * 4);
      uint32_t val = ctx->cpu_ops->read32(ctx->cpu_state, word_addr);
      if (val == 0) continue;
      if (val < ctx->data_seg->p_vaddr)
        ctx->cpu_ops->write32(ctx->cpu_state, word_addr,
                              val + ctx->text_base);
      else
        ctx->cpu_ops->write32(ctx->cpu_state, word_addr,
            val - ctx->data_seg->p_vaddr +
                (uint32_t)(uintptr_t)ctx->data_base);
    }
  }
  if (ctx->data_base &&
      apply_relocations(ctx->ehdr, ctx->file_buf, ctx->file_size,
                        ctx->text_seg, ctx->data_seg, ctx->data_base,
                        ctx->text_base, out->got_sram_addr, &got_info,
                        ctx->cpu_ops, ctx->cpu_state) < 0)
    return -(int)ENOEXEC;
  return 0;
}
#endif /* arch relocation callbacks */

/* Copy data to text memory.  Uses word-at-a-time writes so that this
 * is safe for Xtensa IRAM (which faults on byte stores) while remaining
 * correct on all other architectures. */
static void elf_copy_text(void *dst, const uint8_t *src, uint32_t size) {
  volatile uint32_t *d = (volatile uint32_t *)dst;
  uint32_t words = (size + 3) / 4;
  for (uint32_t w = 0; w < words; w++) {
    uint32_t val = src[w * 4];
    if (w * 4 + 1 < size) val |= (uint32_t)src[w * 4 + 1] << 8;
    if (w * 4 + 2 < size) val |= (uint32_t)src[w * 4 + 2] << 16;
    if (w * 4 + 3 < size) val |= (uint32_t)src[w * 4 + 3] << 24;
    d[w] = val;
  }
}

/* Zero text memory.  Word-at-a-time for Xtensa IRAM safety.
 * Caller must ensure size is a multiple of 4. */
static void elf_zero_text(void *dst, uint32_t size) {
  uint32_t *d = (uint32_t *)dst;
  for (uint32_t w = 0; w < size / 4; w++) d[w] = 0;
}

/* Unified ELF image loader — handles all architectures.
 *
 * 1. Classify segments (text, data, literal on Xtensa)
 * 2. Allocate stacks (kernel + user on m68k/RISC-V)
 * 3. Text: XIP from file buffer or alloc+copy to SRAM
 * 4. Data: alloc+copy to SRAM
 * 5. Arch-specific relocations
 * 6. Page tracking, brk, entry point */
static int elf_load_image(pcb_t *p, const elf32_ehdr_t *ehdr,
                          const uint8_t *file_buf, uint32_t file_size,
                          elf32_phdr_t *segs, int nseg,
                          const cpu_ops_t *cpu_ops, void *cpu_state,
                          elf_load_result_t *out, uint32_t flags) {
  const elf32_phdr_t *text_seg = NULL;
  const elf32_phdr_t *data_seg = NULL;
  const elf32_phdr_t *literal_seg = NULL;

  /* --- 1. Classify segments ---
   * First pass: find text (PF_X).
   * Second pass: writable segments before text are literal (Xtensa L32R
   * pool); others are data.  On non-Xtensa, literal_seg stays NULL. */
  for (int i = 0; i < nseg && i < MAX_LOAD_SEGS; i++) {
    if (segs[i].p_flags & PF_X) text_seg = &segs[i];
  }
  uint32_t data_end_va = 0;
  for (int i = 0; i < nseg && i < MAX_LOAD_SEGS; i++) {
    int is_writable = !!(segs[i].p_flags & PF_W);
    /* R-only segments after text are part of the data region for ePIC
     * (rodata must be GP-accessible from SRAM).  On non-ePIC, only
     * writable segments count as data. */
    int is_ro_after_text = !is_writable && !(segs[i].p_flags & PF_X) &&
                           text_seg &&
                           segs[i].p_vaddr >= text_seg->p_vaddr +
                                                  text_seg->p_memsz;
    if (!is_writable && !is_ro_after_text) continue;
    if (is_writable && text_seg &&
        segs[i].p_vaddr + segs[i].p_memsz <= text_seg->p_vaddr) {
      literal_seg = &segs[i];
      continue;
    }
    if (!data_seg || segs[i].p_vaddr < data_seg->p_vaddr)
      data_seg = &segs[i];
    uint32_t seg_end = segs[i].p_vaddr + segs[i].p_memsz;
    if (seg_end > data_end_va)
      data_end_va = seg_end;
  }
  if (!text_seg) return -(int)ENOEXEC;

  uint32_t e_entry = elf_entry(ehdr);
  elf_text_mode_t text_mode =
      elf_text_mode(!!(flags & EXEC_FLAG_XIP_SOURCE));

  /* Compute text allocation size (includes literal on Xtensa) */
  uint32_t text_end_va = text_seg->p_vaddr + text_seg->p_memsz;
  if (literal_seg &&
      literal_seg->p_vaddr + literal_seg->p_memsz > text_end_va)
    text_end_va = literal_seg->p_vaddr + literal_seg->p_memsz;
  uint32_t text_alloc_size =
      (text_mode == ELF_TEXT_SRAM) ? ((text_end_va + 15u) & ~15u)
                                   : text_seg->p_memsz;

  proc_image_segment_t stack_region = {0};
  proc_image_segment_t ustack_region = {0};
  proc_image_segment_t text_region = {0};
  proc_image_segment_t data_region = {0};

  /* --- 2. Allocate stacks --- */
  if (mem_region_alloc(&stack_region, PPAP_MEM_RAM_STACK, PAGE_SIZE,
                       PROC_IMAGE_SEG_WRITABLE |
                           PROC_IMAGE_SEG_OWNED) < 0) {
    return -(int)ENOMEM;
  }
  p->stack_page_id = mm_ptr_to_page(stack_region.base);
  p->image.stack = stack_region;
  out->stack_top = (uint32_t)(uintptr_t)stack_region.base + PAGE_SIZE;

  /* m68k and RISC-V use a separate user-mode stack */
  int need_user_stack =
      (cpu_ops->arch_id == CPU_ARCH_M68K ||
       cpu_ops->arch_id == CPU_ARCH_RISCV);
  if (need_user_stack) {
    if (mem_region_alloc(&ustack_region, PPAP_MEM_RAM_STACK, PAGE_SIZE,
                         PROC_IMAGE_SEG_WRITABLE |
                             PROC_IMAGE_SEG_OWNED) < 0) {
      mem_region_free(&stack_region);
      p->stack_page_id = PAGE_ID_INVALID;
      return -(int)ENOMEM;
    }
    p->image.stack = ustack_region;
    out->stack_top = (uint32_t)(uintptr_t)ustack_region.base + PAGE_SIZE;
    memset(ustack_region.base, 0, PAGE_SIZE);
  }

  /* --- 3. Text segment: XIP or SRAM --- */
  uint32_t text_base;
  if (text_mode == ELF_TEXT_XIP) {
    text_base = (uint32_t)(uintptr_t)file_buf + text_seg->p_offset;
    p->image.text = proc_image_segment_make(
        (void *)(uintptr_t)text_base, text_seg->p_memsz, PPAP_MEM_ROM_TEXT,
        PROC_IMAGE_SEG_EXECUTABLE | PROC_IMAGE_SEG_XIP);
  } else {
    if (mem_region_alloc(&text_region, PPAP_MEM_RAM_TEXT, text_alloc_size,
                         PROC_IMAGE_SEG_EXECUTABLE |
                             PROC_IMAGE_SEG_OWNED) < 0) {
      mem_region_free(&ustack_region);
      mem_region_free(&stack_region);
      p->stack_page_id = PAGE_ID_INVALID;
      return -(int)ENOMEM;
    }
    uint8_t *text_dst = (uint8_t *)text_region.base;
    text_base = (uint32_t)(uintptr_t)text_dst;

    /* Zero then copy text */
    elf_zero_text(text_dst, text_alloc_size);
    if (text_seg->p_filesz > 0)
      elf_copy_text(text_dst + text_seg->p_vaddr,
                    file_buf + text_seg->p_offset, text_seg->p_filesz);

    /* Copy literal segment into same text region (Xtensa L32R pool) */
    if (literal_seg && literal_seg->p_filesz > 0)
      elf_copy_text(text_dst + literal_seg->p_vaddr,
                    file_buf + literal_seg->p_offset, literal_seg->p_filesz);

    p->image.text = proc_image_segment_make(
        text_dst, text_alloc_size, PPAP_MEM_RAM_TEXT,
        PROC_IMAGE_SEG_EXECUTABLE | PROC_IMAGE_SEG_OWNED);
  }

  if (literal_seg) {
    uint8_t *text_dst = (uint8_t *)(uintptr_t)text_base;
    p->image.literal = proc_image_segment_make_vaddr(
        text_dst + literal_seg->p_vaddr, literal_seg->p_memsz,
        literal_seg->p_vaddr, PPAP_MEM_RAM_RODATA, 0u);
  }

  /* Entry point — Thumb bit preserved for ARM */
  if (cpu_ops->arch_id == CPU_ARCH_M68K ||
      cpu_ops->arch_id == CPU_ARCH_XTENSA ||
      cpu_ops->arch_id == CPU_ARCH_RISCV) {
    out->entry = text_base + e_entry - text_seg->p_vaddr;
  } else {
    /* ARM: preserve Thumb interwork bit */
    out->entry = text_base + (e_entry & ~1u) - text_seg->p_vaddr;
    out->entry |= (e_entry & 1u);
  }
  p->image.entry = out->entry;

  /* --- 4. Data segment: always SRAM ---
   * When the linker produces multiple writable PT_LOAD segments (e.g.
   * .data.rel.ro + .sdata/.data/.bss), allocate a single block spanning
   * from the first writable segment to the end of the last one and copy
   * each segment's file data into the correct offset. */
  uint8_t *data_base = NULL;
  uint32_t data_pages = 0;
  uint32_t data_va = data_seg ? data_seg->p_vaddr : text_end_va;
  uint32_t data_memsz = data_seg ? (data_end_va - data_va) : 0;

  out->got_sram_addr = 0;

  if (data_seg && data_memsz > 0) {
    data_pages = (data_memsz + PAGE_SIZE - 1) / PAGE_SIZE;
    if (data_pages > USER_PAGES_MAX) {
      mem_region_free(&text_region);
      mem_region_free(&ustack_region);
      mem_region_free(&stack_region);
      p->stack_page_id = PAGE_ID_INVALID;
      return -(int)ENOMEM;
    }

    if (mem_region_alloc(&data_region, PPAP_MEM_RAM_DATA, data_memsz,
                         PROC_IMAGE_SEG_WRITABLE |
                             PROC_IMAGE_SEG_OWNED) < 0) {
      mem_region_free(&text_region);
      mem_region_free(&ustack_region);
      mem_region_free(&stack_region);
      p->stack_page_id = PAGE_ID_INVALID;
      return -(int)ENOMEM;
    }
    data_base = (uint8_t *)data_region.base;

    /* Zero entire region (covers gaps between segments and BSS) */
    memset(data_base, 0, data_memsz);
    /* Copy file data from each data-region segment (writable segments
     * plus R-only segments after text for ePIC rodata). */
    for (int i = 0; i < nseg && i < MAX_LOAD_SEGS; i++) {
      if (segs[i].p_flags & PF_X) continue;          /* skip text */
      if (literal_seg && &segs[i] == literal_seg) continue;
      if (segs[i].p_vaddr < data_va) continue;        /* before data */
      if (segs[i].p_vaddr >= data_va + data_memsz) continue; /* after */
      if (segs[i].p_filesz > 0) {
        uint32_t off = segs[i].p_vaddr - data_va;
        memcpy(data_base + off, file_buf + segs[i].p_offset,
               segs[i].p_filesz);
      }
    }
  }


  /* --- 5. Arch-specific relocations --- */
  {
    elf_reloc_ctx_t rctx = {
        ehdr, file_buf, file_size, text_base, data_base, text_end_va,
        data_va, data_memsz, text_seg, data_seg, cpu_ops, cpu_state};
    int reloc_rc = elf_reloc_arch(&rctx, out);
    if (reloc_rc < 0) {
      mem_region_free(&data_region);
      mem_region_free(&text_region);
      mem_region_free(&ustack_region);
      mem_region_free(&stack_region);
      p->stack_page_id = PAGE_ID_INVALID;
      return reloc_rc;
    }
  }
  /* --- 6. Page tracking --- */
  if (data_base) {
    if (proc_track_page_range(p, 0, data_base, data_pages * PAGE_SIZE) < 0) {
      mem_region_free(&data_region);
      mem_region_free(&text_region);
      mem_region_free(&ustack_region);
      mem_region_free(&stack_region);
      p->stack_page_id = PAGE_ID_INVALID;
      p->image.stack = (proc_image_segment_t){0};
      return -(int)ENOMEM;
    }
    p->image.data = proc_image_segment_make(
        data_base, data_memsz, PPAP_MEM_RAM_DATA,
        PROC_IMAGE_SEG_WRITABLE | PROC_IMAGE_SEG_OWNED);
  }

  /* RISC-V: track user stack in the last user_pages slot to keep it
   * out of the brk growth range (slots data_pages .. USER_PAGES_MAX-2).
   * Derive stack_top from this tracked page. */
  if (cpu_ops->arch_id == CPU_ARCH_RISCV && need_user_stack) {
    if (proc_track_page(p, USER_PAGES_MAX - 1, ustack_region.base) < 0) {
      mem_region_free(&data_region);
      mem_region_free(&text_region);
      mem_region_free(&ustack_region);
      mem_region_free(&stack_region);
      p->stack_page_id = PAGE_ID_INVALID;
      p->image.stack = (proc_image_segment_t){0};
      return -(int)ENOMEM;
    }
    void *us_ptr = proc_last_page_backed_base(p);
    out->stack_top = (uint32_t)(uintptr_t)us_ptr + PAGE_SIZE;
  }

#if defined(__m68k__)
  if (cpu_ops->arch_id == CPU_ARCH_M68K && need_user_stack)
    p->user_stack_page = ustack_region.base;
#endif

  /* --- 7. brk setup --- */
  if (data_base) {
    uint32_t data_end =
        (uint32_t)(uintptr_t)data_base + data_memsz;
    data_end = (data_end + 15u) & ~15u;
    p->brk_base = data_end;
    p->brk_current = data_end;
  } else if (text_mode == ELF_TEXT_SRAM) {
    /* Text in SRAM — brk grows after the text allocation */
    uint32_t text_end = text_base + text_end_va;
    text_end = (text_end + 15u) & ~15u;
    p->brk_base = text_end;
    p->brk_current = text_end;
  } else {
    /* XIP text, no data — no writable region for heap.
     * brk stays 0; sys_brk will allocate fresh pages on demand. */
    p->brk_base = 0;
    p->brk_current = 0;
  }

  /* Effective load base for gp computation (ePIC RISC-V).
   * With separate text/data allocations, load_base = data_base - data_va
   * so that load_base + __global_pointer$ resolves correctly. */
  if (data_base && data_seg)
    out->load_base = (uint32_t)(uintptr_t)data_base - data_seg->p_vaddr;
  else
    out->load_base = text_base;

  return 0;
}

static int elf_load(pcb_t *p, const uint8_t *file_buf, uint32_t file_size,
                    const cpu_ops_t *cpu_ops, void *cpu_state,
                    const char *const *argv, uint32_t flags) {
  /* Create CPU state if not provided by coordinator */
  int own_state = 0;
  if (!cpu_state) {
    cpu_state = cpu_ops->create_state();
    if (!cpu_state) return -(int)ENOMEM;
    cpu_ops->init(cpu_state, (uint8_t *)0, 0xFFFFFFFF);
    own_state = 1;
  }
  (void)own_state;

  const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)file_buf;
  elf32_phdr_t segs[MAX_LOAD_SEGS];
  int nseg = elf_load_segments(ehdr, file_buf, segs, MAX_LOAD_SEGS, file_size);
  if (nseg <= 0) return -(int)ENOEXEC;

  p->image = (proc_image_t){0};

  elf_load_result_t res = {0};
  int rc = elf_load_image(p, ehdr, file_buf, file_size, segs, nseg,
                          cpu_ops, cpu_state, &res, flags);
  if (rc < 0) return rc;

  uint32_t entry = res.entry;
  uint32_t got_sram_addr = res.got_sram_addr;
  uint32_t argv_sp = 0;
  uint32_t sp = res.stack_top;

  int argc = 0;
  while (argv && argv[argc]) {
    argc++;
    if (argc > 15) break;
  }

  uint32_t str_addrs[16];
  for (int i = argc - 1; i >= 0; i--) {
    uint32_t len = (uint32_t)strlen(argv[i]) + 1;
    sp -= len;
    memcpy((void *)(uintptr_t)sp, argv[i], len);
    str_addrs[i] = sp;
  }

  /* Align sp after string copies: 4-byte for m68k, 8-byte for ARM/RISC-V.
   * The extra pad on non-m68k ensures sp stays 8-byte aligned after
   * pushing the fixed-size frame below (argc + argv[] + terminators +
   * auxv = argc+7 words; pad an extra word if the count is odd). */
  if (cpu_ops->arch_id == CPU_ARCH_M68K) {
    sp &= ~3u;
  } else {
    sp &= ~7u;
    if ((argc + 7) & 1) sp -= 4;
  }

  /* Build initial stack frame (grows downward).  Layout matches what
   * crt0 / _start expects on entry:
   *
   *   sp -> argc
   *         argv[0]  argv[1]  ...  argv[argc-1]
   *         0        (argv terminator / NULL)
   *         0        (envp terminator / NULL)
   *         AT_PAGESZ  PAGE_SIZE   (ELF auxiliary vector)
   *         AT_NULL    0           (aux terminator)
   *
   * Pushed in reverse order since the stack grows down. */
  sp -= 4;
  *(uint32_t *)(uintptr_t)sp = 0;         /* AT_NULL value */
  sp -= 4;
  *(uint32_t *)(uintptr_t)sp = 0;         /* AT_NULL tag */
  sp -= 4;
  *(uint32_t *)(uintptr_t)sp = PAGE_SIZE; /* AT_PAGESZ value */
  sp -= 4;
  *(uint32_t *)(uintptr_t)sp = 6;         /* AT_PAGESZ tag */
  sp -= 4;
  *(uint32_t *)(uintptr_t)sp = 0;         /* envp terminator */
  sp -= 4;
  *(uint32_t *)(uintptr_t)sp = 0;         /* argv terminator */
  for (int i = argc - 1; i >= 0; i--) {
    sp -= 4;
    *(uint32_t *)(uintptr_t)sp = str_addrs[i];  /* argv[i] */
  }
  sp -= 4;
  *(uint32_t *)(uintptr_t)sp = (uint32_t)argc;  /* argc */
  argv_sp = sp;

  if (cpu_ops->arch_id == CPU_ARCH_M68K) {
    extern volatile int exec_pending[2];
    uint32_t irq_save = arch_irq_save();
    proc_setup_stack(p, (void (*)(void))(uintptr_t)entry, argv_sp);
#if defined(__m68k__)
    p->usp = argv_sp;
#endif
    if (got_sram_addr) {
      uint32_t *sw = (uint32_t *)(uintptr_t)p->sp;
      sw[13] = got_sram_addr;
    }
    p->got_base = got_sram_addr;
    if (p == current) exec_pending[0] = 1;
    arch_irq_restore(irq_save);
  } else {
    proc_setup_stack(p, (void (*)(void))(uintptr_t)entry, argv_sp);
    {
      uint32_t *sw = (uint32_t *)(uintptr_t)p->sp;
      if (got_sram_addr && cpu_ops->arch_id != CPU_ARCH_XTENSA) {
        /* ARM/RISC-V (GCC PIC): patch PIC register to GOT address.
         * Xtensa: GOT via L32R (PC-relative literals), no register. */
        sw[5] = got_sram_addr; /* ARM r9 */
      }
      if (cpu_ops->arch_id == CPU_ARCH_RISCV && !got_sram_addr &&
          ehdr->e_type == ET_DYN && res.load_base) {
        /* RISC-V ePIC: ET_DYN (PIE) with no GOT — gp-relative data.
         * Set gp = load_base so crt0 can bootstrap __global_pointer$.
         * load_base = data_base - data_va, accounting for separate
         * text/data allocations. */
        sw[1] = res.load_base;
      }
    }
    p->got_base = got_sram_addr;
  }

  if (p->image.text.flags & PROC_IMAGE_SEG_XIP)
    p->image.flags |= PROC_IMAGE_FLAG_TEXT_XIP;

  return 0;
}

const loader_t elf_loader = {
    .name = "elf",
    .detect = elf_detect,
    .load = elf_load,
    .required_arch_id = 0,
    .xip = 1,
};
