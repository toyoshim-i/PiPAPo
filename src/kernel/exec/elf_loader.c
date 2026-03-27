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

#if !defined(__riscv) && !defined(__xtensa__)
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
      if (sym_val < data_seg->p_vaddr) {
        cpu_ops->write32(cpu_state, word_addr, sym_val + text_base);
      } else {
        cpu_ops->write32(
            cpu_state, word_addr,
            sym_val - data_seg->p_vaddr + (uint32_t)(uintptr_t)sram_page);
      }
      continue;
    }

    if ((cpu_ops->arch_id == CPU_ARCH_M68K && rtype != R_68K_RELATIVE &&
         rtype != R_68K_JMP_SLOT) ||
        (cpu_ops->arch_id == CPU_ARCH_ARM && rtype != R_ARM_RELATIVE &&
         rtype != R_ARM_JMP_SLOT) ||
        (cpu_ops->arch_id == CPU_ARCH_ARMV6 && rtype != R_ARM_RELATIVE &&
         rtype != R_ARM_JMP_SLOT) ||
        (cpu_ops->arch_id == CPU_ARCH_XTENSA && rtype != R_XTENSA_RELATIVE)) {
      klogf("ELF: unhandled reloc type %u at offset %x\n", rtype, r_offset);
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

    if (val < data_seg->p_vaddr) {
      cpu_ops->write32(cpu_state, word_addr, val + text_base);
    } else {
      cpu_ops->write32(
          cpu_state, word_addr,
          val - data_seg->p_vaddr + (uint32_t)(uintptr_t)sram_page);
    }
  }
  return 0;
}
#endif /* !__riscv && !__xtensa__ */

static int elf_detect(const uint8_t *file_buf, uint32_t file_size,
                      const char *path) {
  (void)path;
  if (file_size < sizeof(elf32_ehdr_t)) return 0;
  const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)file_buf;
  return elf_validate(ehdr) == 0;
}

#if defined(__xtensa__)
typedef struct {
  uint32_t blocked_offset;
  uint32_t blocked_type;
  uint32_t text_blocked_count;
  uint32_t literal_slot0_count;
  uint32_t literal32_count;
  uint32_t literal_data_count;
  uint32_t literal_prelinked;
} xtensa_xip_report_t;

typedef enum {
  XTENSA_XIP_STATE_TEXT_BLOCKED = 0,
  XTENSA_XIP_STATE_DATA_COUPLED,
  XTENSA_XIP_STATE_LITERAL_PRELINKED,
  XTENSA_XIP_STATE_LITERAL_COUPLED,
  XTENSA_XIP_STATE_READY,
} xtensa_xip_state_t;

static const elf32_shdr_t *elf_section_by_name(const elf32_ehdr_t *ehdr,
                                               const uint8_t *file_base,
                                               uint32_t file_size,
                                               const char *name) {
  const elf32_shdr_t *shstr;
  const char *shstrtab;

  if (!ehdr->e_shoff || !ehdr->e_shnum || !ehdr->e_shentsize) return NULL;
  if (ehdr->e_shstrndx >= ehdr->e_shnum) return NULL;

  shstr = (const elf32_shdr_t *)(file_base + ehdr->e_shoff +
                                 ehdr->e_shstrndx * ehdr->e_shentsize);
  if (shstr->sh_offset >= file_size ||
      shstr->sh_offset + shstr->sh_size > file_size)
    return NULL;
  shstrtab = (const char *)(file_base + shstr->sh_offset);

  for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
    const elf32_shdr_t *sh = (const elf32_shdr_t *)(file_base + ehdr->e_shoff +
                                                    i * ehdr->e_shentsize);
    if (sh->sh_name >= shstr->sh_size) continue;
    if (strcmp(shstrtab + sh->sh_name, name) == 0) return sh;
  }
  return NULL;
}

static int xtensa_elf_uses_xip_layout(const elf32_ehdr_t *ehdr,
                                      const uint8_t *file_base,
                                      uint32_t file_size,
                                      const elf32_phdr_t *text_seg) {
  const elf32_shdr_t *rodata =
      elf_section_by_name(ehdr, file_base, file_size, ".rodata");
  uint32_t text_start;
  uint32_t text_end;

  if (!rodata || !text_seg) return 0;

  text_start = text_seg->p_vaddr;
  text_end = text_start + text_seg->p_memsz;
  return rodata->sh_addr >= text_start && rodata->sh_addr < text_end;
}

static int xtensa_rela_symbol_in_range(const elf32_ehdr_t *ehdr,
                                       const uint8_t *file_base,
                                       uint32_t file_size,
                                       const uint8_t *rela_sh,
                                       uint32_t r_info,
                                       uint32_t start,
                                       uint32_t end) {
  uint32_t symtab_idx = *(const uint32_t *)(rela_sh + 24);
  uint32_t sym_idx = ELF32_R_SYM(r_info);
  const elf32_shdr_t *symtab;
  const elf32_sym_t *sym;
  uint32_t sym_count;

  if (symtab_idx >= ehdr->e_shnum) return 0;

  symtab = (const elf32_shdr_t *)(file_base + ehdr->e_shoff +
                                  symtab_idx * ehdr->e_shentsize);
  if (symtab->sh_entsize < sizeof(elf32_sym_t)) return 0;
  if (symtab->sh_offset >= file_size ||
      symtab->sh_offset + symtab->sh_size > file_size)
    return 0;

  sym_count = symtab->sh_size / symtab->sh_entsize;
  if (sym_idx >= sym_count) return 0;

  sym = (const elf32_sym_t *)(file_base + symtab->sh_offset +
                              sym_idx * symtab->sh_entsize);
  return sym->st_value >= start && sym->st_value < end;
}

static int xtensa_elf_symbol_value_by_name(const elf32_ehdr_t *ehdr,
                                           const uint8_t *file_base,
                                           uint32_t file_size,
                                           const char *name,
                                           uint32_t *value) {
  if (!ehdr->e_shoff || !ehdr->e_shnum || !ehdr->e_shentsize) return 0;

  for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
    const elf32_shdr_t *symtab;
    const elf32_shdr_t *strtab;
    uint32_t n_syms;

    symtab = (const elf32_shdr_t *)(file_base + ehdr->e_shoff +
                                    i * ehdr->e_shentsize);
    if (symtab->sh_type != 2 /* SHT_SYMTAB */) continue;
    if (symtab->sh_link >= ehdr->e_shnum) continue;
    if (symtab->sh_offset >= file_size ||
        symtab->sh_offset + symtab->sh_size > file_size)
      continue;
    if (symtab->sh_entsize < sizeof(elf32_sym_t)) continue;

    strtab = (const elf32_shdr_t *)(file_base + ehdr->e_shoff +
                                    symtab->sh_link * ehdr->e_shentsize);
    if (strtab->sh_offset >= file_size ||
        strtab->sh_offset + strtab->sh_size > file_size)
      continue;

    n_syms = symtab->sh_size / symtab->sh_entsize;
    for (uint32_t si = 0; si < n_syms; si++) {
      const elf32_sym_t *sym = (const elf32_sym_t *)(file_base +
                                                     symtab->sh_offset +
                                                     si * symtab->sh_entsize);
      const char *sym_name;

      if (sym->st_name >= strtab->sh_size) continue;
      sym_name = (const char *)(file_base + strtab->sh_offset + sym->st_name);
      if (strcmp(sym_name, name) != 0) continue;
      if (value) *value = sym->st_value;
      return 1;
    }
  }

  return 0;
}

static int xtensa_literal_word_is_prelinked(const uint8_t *file_base,
                                            uint32_t file_size,
                                            const elf32_phdr_t *literal_seg,
                                            uint32_t literal_start_va,
                                            uint32_t r_offset,
                                            uint32_t flash_base) {
  uint32_t flash_end = flash_base + 0x01000000u;
  uint32_t file_off;
  const uint8_t *p;
  uint32_t word;

  if (!literal_seg) return 0;
  if (r_offset < literal_start_va ||
      r_offset + 4 > literal_start_va + literal_seg->p_filesz)
    return 0;

  file_off = literal_seg->p_offset + (r_offset - literal_start_va);
  if (file_off + 4 > file_size) return 0;

  p = file_base + file_off;
  word = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  return word >= flash_base && word < flash_end;
}

static xtensa_xip_state_t xtensa_elf_xip_state(const elf32_ehdr_t *ehdr,
                                               const uint8_t *file_base,
                                               uint32_t file_size,
                                               const elf32_phdr_t *text_seg,
                                               const elf32_phdr_t *literal_seg,
                                               const elf32_phdr_t *data_seg,
                                               xtensa_xip_report_t *report) {
  uint32_t text_start_va;
  uint32_t text_end_va;
  uint32_t literal_start_va = 0;
  uint32_t literal_end_va = 0;
  uint32_t data_start_va = 0;
  uint32_t data_end_va = 0;
  uint32_t flash_base = 0;
  uint32_t prelinked_refs = 0;
  int have_flash_base;
  int literal_prelinked_ok = 1;

  if (report) *report = (xtensa_xip_report_t){0};
  if (!text_seg) return XTENSA_XIP_STATE_TEXT_BLOCKED;

  text_start_va = text_seg->p_vaddr;
  text_end_va = text_start_va + text_seg->p_memsz;
  if (literal_seg) {
    literal_start_va = literal_seg->p_vaddr;
    literal_end_va = literal_start_va + literal_seg->p_memsz;
  }
  if (data_seg) {
    data_start_va = data_seg->p_vaddr;
    data_end_va = data_start_va + data_seg->p_memsz;
  }
  have_flash_base = xtensa_elf_symbol_value_by_name(
      ehdr, file_base, file_size, "__ppap_xip_flash_base", &flash_base);
  if (!have_flash_base || flash_base == 0) literal_prelinked_ok = 0;

  if (!ehdr->e_shoff || !ehdr->e_shnum || ehdr->e_shentsize < 40)
    return XTENSA_XIP_STATE_READY;

  for (uint16_t si = 0; si < ehdr->e_shnum; si++) {
    const uint8_t *sh = file_base + ehdr->e_shoff + si * ehdr->e_shentsize;
    uint32_t sh_type = *(const uint32_t *)(sh + 4);
    uint32_t sh_info_idx;
    const uint8_t *target_sh;
    uint32_t target_flags;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_entsize;
    uint32_t n_entries;

    if (sh_type != 4 /* SHT_RELA */) continue;

    sh_info_idx = *(const uint32_t *)(sh + 28);
    if (sh_info_idx >= ehdr->e_shnum) continue;
    target_sh = file_base + ehdr->e_shoff + sh_info_idx * ehdr->e_shentsize;
    target_flags = *(const uint32_t *)(target_sh + 8);
    if (!(target_flags & 2 /* SHF_ALLOC */)) continue;

    sh_offset = *(const uint32_t *)(sh + 16);
    sh_size = *(const uint32_t *)(sh + 20);
    sh_entsize = *(const uint32_t *)(sh + 36);
    if (sh_entsize < 12 || sh_offset + sh_size > file_size) continue;

    n_entries = sh_size / sh_entsize;
    for (uint32_t ri = 0; ri < n_entries; ri++) {
      const uint8_t *rela = file_base + sh_offset + ri * sh_entsize;
      uint32_t r_offset = *(const uint32_t *)(rela + 0);
      uint32_t r_info = *(const uint32_t *)(rela + 4);
      uint8_t r_type = (uint8_t)(r_info & 0xFFu);

      if (r_type != 1 /* R_XTENSA_32 */ &&
          r_type != 20 /* R_XTENSA_SLOT0_OP */ &&
          r_type != 6 /* R_XTENSA_PLT */)
        continue;

      if ((r_type == 1 || r_type == 6) &&
          r_offset >= text_start_va && r_offset < text_end_va) {
        if (report) {
          if (report->text_blocked_count == 0) {
            report->blocked_offset = r_offset;
            report->blocked_type = r_type;
          }
          report->text_blocked_count++;
        }
        continue;
      }

      if (!literal_seg) continue;

      if (r_type == 1 &&
          r_offset >= literal_start_va && r_offset < literal_end_va) {
        if (report) report->literal32_count++;
        if (data_seg &&
            xtensa_rela_symbol_in_range(ehdr, file_base, file_size, sh, r_info,
                                        data_start_va, data_end_va)) {
          if (report) report->literal_data_count++;
          continue;
        }
        if (literal_prelinked_ok) {
          prelinked_refs++;
          if (!xtensa_literal_word_is_prelinked(file_base, file_size,
                                                literal_seg, literal_start_va,
                                                r_offset, flash_base))
            literal_prelinked_ok = 0;
        }
        continue;
      }

      if (r_type == 20 /* R_XTENSA_SLOT0_OP */ &&
          r_offset >= text_start_va && r_offset < text_end_va &&
          xtensa_rela_symbol_in_range(ehdr, file_base, file_size, sh, r_info,
                                      literal_start_va, literal_end_va)) {
        if (report) report->literal_slot0_count++;
      }
    }
  }

  if (report && report->text_blocked_count > 0)
    return XTENSA_XIP_STATE_TEXT_BLOCKED;
  if (report && report->literal_data_count > 0)
    return XTENSA_XIP_STATE_DATA_COUPLED;
  if (report) report->literal_prelinked = 0u;
  if (prelinked_refs > 0 && literal_prelinked_ok) {
    if (report) report->literal_prelinked = 1u;
    return XTENSA_XIP_STATE_LITERAL_PRELINKED;
  }
  if (report &&
      (report->literal_slot0_count > 0 || report->literal32_count > 0))
    return XTENSA_XIP_STATE_LITERAL_COUPLED;
  return XTENSA_XIP_STATE_READY;
}
#endif

#define MAX_LOAD_SEGS 4

static int elf_load(pcb_t *p, const uint8_t *file_buf, uint32_t file_size,
                    const cpu_ops_t *cpu_ops, void *cpu_state,
                    const char *const *argv) {
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

  const elf32_phdr_t *text_seg = NULL;
  const elf32_phdr_t *data_seg = NULL;
#if defined(__xtensa__)
  const elf32_phdr_t *literal_seg = NULL;
#endif

#if defined(__xtensa__)
  for (int i = 0; i < nseg && i < MAX_LOAD_SEGS; i++) {
    if (segs[i].p_flags & PF_X) text_seg = &segs[i];
  }

  for (int i = 0; i < nseg && i < MAX_LOAD_SEGS; i++) {
    if (!(segs[i].p_flags & PF_W)) continue;
    if (text_seg &&
        segs[i].p_vaddr + segs[i].p_memsz <= text_seg->p_vaddr) {
      literal_seg = &segs[i];
      continue;
    }
    if (!data_seg || segs[i].p_vaddr < data_seg->p_vaddr)
      data_seg = &segs[i];
  }
#else
  for (int i = 0; i < nseg && i < MAX_LOAD_SEGS; i++) {
    if (segs[i].p_flags & PF_X)
      text_seg = &segs[i];
    else if (segs[i].p_flags & PF_W)
      data_seg = &segs[i];
  }
#endif

  if (!text_seg) return -(int)ENOEXEC;

  uint32_t e_entry = elf_entry(ehdr);
  uint32_t entry;
  uint8_t *sram_page = NULL;
  uint32_t got_sram_addr = 0;
#if !defined(__riscv) && !defined(__xtensa__)
  elf_got_info_t got_info = {0, 0, 0};
#endif

  /* On RISC-V/Xtensa, the contiguous SRAM block must be allocated BEFORE
   * the stack page.  Otherwise the stack could land right after the
   * contiguous block, blocking sys_brk's page_alloc_at for heap growth. */
  void *stack = NULL;
#if !defined(__riscv)
  void *user_stack = NULL;
#endif

  p->image = (proc_image_t){0};

#if !defined(__riscv) && !defined(__xtensa__)
  uint32_t xip_text_base = (uint32_t)(uintptr_t)file_buf + text_seg->p_offset;
#endif

#if defined(__xtensa__)
  /* ESP32-S3 split memory:
   *   IRAM (0x4037xxxx): executable, word-access only (no byte load/store)
   *   DRAM (0x3FC9xxxx): byte-accessible, NOT executable
   *
   * Text segment → IRAM (via heap_caps_malloc EXEC)
   * Data segment (rodata + GOT + .data + .bss) → DRAM (via page_alloc)
   *
   * Relocations are split: link-time addresses in the text range get the
   * IRAM base, addresses in the data range get the DRAM base. */
  {
    proc_image_segment_t text_region = {0};
    proc_image_segment_t staged_text_region = {0};
    proc_image_segment_t staged_rodata_region = {0};
    proc_image_segment_t data_region = {0};
    proc_image_segment_t stack_region = {0};
    uint32_t text_end_va = text_seg->p_vaddr + text_seg->p_memsz;
    if (literal_seg &&
        literal_seg->p_vaddr + literal_seg->p_memsz > text_end_va)
      text_end_va = literal_seg->p_vaddr + literal_seg->p_memsz;
    uint32_t text_size = (text_end_va + 15u) & ~15u;
    uint32_t data_va = data_seg ? data_seg->p_vaddr : text_end_va;
    uint32_t data_memsz = data_seg ? data_seg->p_memsz : 0;
    uint32_t data_size = (data_memsz + 15u) & ~15u;
    int uses_xip_layout =
        xtensa_elf_uses_xip_layout(ehdr, file_buf, file_size, text_seg);
    xtensa_xip_report_t xip_report = {0};
    xtensa_xip_state_t xip_state = XTENSA_XIP_STATE_TEXT_BLOCKED;

    if (uses_xip_layout) {
      xip_state = xtensa_elf_xip_state(ehdr, file_buf, file_size, text_seg,
                                       literal_seg, data_seg, &xip_report);
      if (xip_state == XTENSA_XIP_STATE_TEXT_BLOCKED) {
        klogf("ELF: xtensa XIP layout blocked by text reloc type %u at %x\n",
              xip_report.blocked_type, xip_report.blocked_offset);
      } else if (xip_state == XTENSA_XIP_STATE_DATA_COUPLED) {
        klogf("ELF: xtensa XIP layout remains data-coupled"
              " (slot0=%u literal32=%u data32=%u)\n",
              xip_report.literal_slot0_count, xip_report.literal32_count,
              xip_report.literal_data_count);
      } else if (xip_state == XTENSA_XIP_STATE_LITERAL_PRELINKED) {
        klogf("ELF: xtensa XIP layout is literal-prelinked"
              " (slot0=%u literal32=%u)\n",
              xip_report.literal_slot0_count,
              xip_report.literal32_count);
      } else if (xip_state == XTENSA_XIP_STATE_LITERAL_COUPLED) {
        klogf("ELF: xtensa XIP layout remains literal-coupled"
              " (slot0=%u literal32=%u)\n",
              xip_report.literal_slot0_count,
              xip_report.literal32_count);
      }
    }

    /* Allocate IRAM for text (executable, word-access only) */
    if (mem_region_alloc(&text_region, PPAP_MEM_RAM_TEXT, text_size,
                         PROC_IMAGE_SEG_EXECUTABLE |
                             PROC_IMAGE_SEG_OWNED) < 0) {
      return -(int)ENOMEM;
    }
    sram_page = (uint8_t *)text_region.base;

    /* Allocate DRAM for data (byte-accessible, needed for .rodata strings) */
    uint8_t *dram_page = NULL;
    uint32_t data_pages = 0;
    if (data_size > 0) {
      data_pages = (data_size + PAGE_SIZE - 1) / PAGE_SIZE;
      if (mem_region_alloc(&data_region, PPAP_MEM_RAM_DATA, data_size,
                           PROC_IMAGE_SEG_WRITABLE |
                               PROC_IMAGE_SEG_OWNED) < 0) {
        mem_region_free(&text_region);
        return -(int)ENOMEM;
      }
      dram_page = (uint8_t *)data_region.base;
    }

    /* Allocate kernel stack page */
    if (mem_region_alloc(&stack_region, PPAP_MEM_RAM_STACK, PAGE_SIZE,
                         PROC_IMAGE_SEG_WRITABLE |
                             PROC_IMAGE_SEG_OWNED) < 0) {
      mem_region_free(&data_region);
      mem_region_free(&text_region);
      return -(int)ENOMEM;
    }
    stack = stack_region.base;
    p->stack_page = stack;
    p->image.stack = stack_region;

    if (uses_xip_layout &&
        mem_region_alloc(&staged_text_region, PPAP_MEM_EXT_TEXT,
                         text_seg->p_memsz,
                         PROC_IMAGE_SEG_EXECUTABLE |
                             PROC_IMAGE_SEG_OWNED) == 0) {
      uint8_t *staged_text = (uint8_t *)staged_text_region.base;
      for (uint32_t i = 0; i < text_seg->p_memsz; i++)
        staged_text[i] = 0;

      if (text_seg->p_filesz > 0) {
        const uint8_t *src = file_buf + text_seg->p_offset;
        uint8_t *dst = staged_text;
        for (uint32_t i = 0; i < text_seg->p_filesz; i++)
          dst[i] = src[i];
      }

      p->image.staged_text = proc_image_segment_make_vaddr(
          staged_text_region.base, staged_text_region.size, text_seg->p_vaddr,
          PPAP_MEM_EXT_TEXT, staged_text_region.flags);
      p->image.flags |= PROC_IMAGE_FLAG_TEXT_STAGED_EXT;
    }

    if (uses_xip_layout && literal_seg &&
        mem_region_alloc(&staged_rodata_region, PPAP_MEM_EXT_RODATA,
                         literal_seg->p_memsz, PROC_IMAGE_SEG_OWNED) == 0) {
      uint8_t *staged_rodata = (uint8_t *)staged_rodata_region.base;
      for (uint32_t i = 0; i < literal_seg->p_memsz; i++)
        staged_rodata[i] = 0;

      if (literal_seg->p_filesz > 0) {
        const uint8_t *src = file_buf + literal_seg->p_offset;
        uint8_t *dst = staged_rodata;
        for (uint32_t i = 0; i < literal_seg->p_filesz; i++)
          dst[i] = src[i];
      }

      p->image.staged_rodata = proc_image_segment_make_vaddr(
          staged_rodata_region.base, staged_rodata_region.size,
          literal_seg->p_vaddr, PPAP_MEM_EXT_RODATA,
          staged_rodata_region.flags);
    }

    /* Zero IRAM (word-at-a-time — byte memset crashes on IRAM) */
    {
      uint32_t *dst32 = (uint32_t *)(uintptr_t)sram_page;
      for (uint32_t w = 0; w < text_size / 4; w++)
        dst32[w] = 0;
    }

    /* Zero DRAM (byte access OK) */
    if (dram_page) {
      uint8_t *d = dram_page;
      for (uint32_t i = 0; i < data_size; i++)
        d[i] = 0;
    }

    /* Copy text segment to IRAM (word-at-a-time) */
    if (text_seg->p_filesz > 0) {
      const uint8_t *src = file_buf + text_seg->p_offset;
      volatile uint32_t *dst =
          (volatile uint32_t *)(uintptr_t)(sram_page + text_seg->p_vaddr);
      uint32_t words = (text_seg->p_filesz + 3) / 4;
      for (uint32_t w = 0; w < words; w++) {
        uint32_t val = src[w * 4];
        if (w * 4 + 1 < text_seg->p_filesz)
          val |= (uint32_t)src[w * 4 + 1] << 8;
        if (w * 4 + 2 < text_seg->p_filesz)
          val |= (uint32_t)src[w * 4 + 2] << 16;
        if (w * 4 + 3 < text_seg->p_filesz)
          val |= (uint32_t)src[w * 4 + 3] << 24;
        dst[w] = val;
      }
    }

    /* Copy separate Xtensa literal support segment when present.
     * This keeps the RAM-loaded fallback compatible with the experimental
     * XIP packaging, even though the runtime still stages text into IRAM. */
    if (literal_seg && literal_seg->p_filesz > 0) {
      const uint8_t *src = file_buf + literal_seg->p_offset;
      volatile uint32_t *dst =
          (volatile uint32_t *)(uintptr_t)(sram_page + literal_seg->p_vaddr);
      uint32_t words = (literal_seg->p_filesz + 3) / 4;
      for (uint32_t w = 0; w < words; w++) {
        uint32_t val = src[w * 4];
        if (w * 4 + 1 < literal_seg->p_filesz)
          val |= (uint32_t)src[w * 4 + 1] << 8;
        if (w * 4 + 2 < literal_seg->p_filesz)
          val |= (uint32_t)src[w * 4 + 2] << 16;
        if (w * 4 + 3 < literal_seg->p_filesz)
          val |= (uint32_t)src[w * 4 + 3] << 24;
        dst[w] = val;
      }
    }

    /* Copy data segment to DRAM (byte access OK) */
    if (dram_page && data_seg && data_seg->p_filesz > 0) {
      const uint8_t *src = file_buf + data_seg->p_offset;
      uint8_t *dst = dram_page;
      for (uint32_t i = 0; i < data_seg->p_filesz; i++)
        dst[i] = src[i];
    }

    uint32_t iram_base = (uint32_t)(uintptr_t)sram_page;
    uint32_t dram_base = dram_page ? (uint32_t)(uintptr_t)dram_page : 0;

    /* XT-2.6: Determine active execution path based on XIP readiness.
     * - If ENABLE_XTENSA_PSRAM_EXEC is set AND staged text exists (XIP-capable
     *   layout), use PSRAM-backed execution base for entry point.
     * - Otherwise, fall back to IRAM-loaded execution (current behavior). */
#if defined(ENABLE_XTENSA_PSRAM_EXEC)
    uint32_t active_text_base = iram_base;
    if ((p->image.flags & PROC_IMAGE_FLAG_TEXT_STAGED_EXT) &&
        p->image.staged_text.base) {
      active_text_base = (uint32_t)(uintptr_t)p->image.staged_text.base;
    }
#else
    uint32_t active_text_base = iram_base;
#endif

    /* Entry point calculation based on active execution path */
    entry = active_text_base + e_entry;
    p->image.text = proc_image_segment_make_vaddr(
        text_region.base, text_region.size, text_seg->p_vaddr,
        PPAP_MEM_RAM_TEXT, text_region.flags);
    if (literal_seg) {
      /* The RAM-loaded fallback still places .literal inside the same
       * IRAM allocation for L32R reach, but model it as read-mostly
       * support data so the future XIP path can lift it out of text. */
      p->image.literal = proc_image_segment_make_vaddr(
          sram_page + literal_seg->p_vaddr, literal_seg->p_memsz,
          literal_seg->p_vaddr, PPAP_MEM_RAM_RODATA, 0u);
    }
    p->image.entry = entry;
    if (uses_xip_layout && xip_state == XTENSA_XIP_STATE_LITERAL_COUPLED)
      p->image.flags |= PROC_IMAGE_FLAG_LITERAL_COUPLED;
    if (uses_xip_layout && xip_state == XTENSA_XIP_STATE_LITERAL_PRELINKED)
      p->image.flags |= PROC_IMAGE_FLAG_LITERAL_PRELINKED;
    if (uses_xip_layout && xip_state == XTENSA_XIP_STATE_DATA_COUPLED)
      p->image.flags |= PROC_IMAGE_FLAG_DATA_COUPLED;

#if defined(ENABLE_XTENSA_PSRAM_EXEC)
    if ((p->image.flags & PROC_IMAGE_FLAG_TEXT_STAGED_EXT) &&
        p->image.staged_text.base) {
      klogf("ELF: xtensa entry 0x%x (PSRAM @ 0x%lx)\n", entry,
            (uintptr_t)p->image.staged_text.base);
    } else if (uses_xip_layout) {
      klogf("ELF: xtensa entry 0x%x (IRAM fallback @ 0x%x)\n", entry,
            iram_base);
    }
#endif

    /* Apply split relocations from --emit-relocs sections.
     *
     * Link-time layout: text at vaddr [0, text_end_va), data at [data_va, ...).
     * Runtime: text at active_text_base (IRAM or PSRAM), data at dram_base.
     *
     * For each R_XTENSA_32/PLT relocation, the patched word contains a
     * link-time address V.  Convert to runtime:
     *   V < data_va  → active_text_base + V  (text/code reference)
     *   V >= data_va → dram_base + (V - data_va) (data/rodata reference)
     *
     * IMPORTANT: only process RELA sections whose target section (sh_info)
     * has SHF_ALLOC set.  Metadata sections like .xt.prop and .xt.lit have
     * their own RELA sections with section-internal offsets. */
    uint32_t image_end = data_va + data_memsz;
    if (ehdr->e_shoff && ehdr->e_shnum && ehdr->e_shentsize >= 40) {
      for (uint16_t si = 0; si < ehdr->e_shnum; si++) {
        const uint8_t *sh =
            file_buf + ehdr->e_shoff + si * ehdr->e_shentsize;
        uint32_t sh_type = *(const uint32_t *)(sh + 4);
        if (sh_type != 4 /* SHT_RELA */) continue;

        uint32_t sh_info_idx = *(const uint32_t *)(sh + 28);
        if (sh_info_idx >= ehdr->e_shnum) continue;
        const uint8_t *target_sh =
            file_buf + ehdr->e_shoff + sh_info_idx * ehdr->e_shentsize;
        uint32_t target_flags = *(const uint32_t *)(target_sh + 8);
        if (!(target_flags & 2 /* SHF_ALLOC */)) continue;

        uint32_t sh_offset  = *(const uint32_t *)(sh + 16);
        uint32_t sh_size    = *(const uint32_t *)(sh + 20);
        uint32_t sh_entsize = *(const uint32_t *)(sh + 36);
        if (sh_entsize < 12) continue;

        uint32_t n_entries = sh_size / sh_entsize;
        for (uint32_t ri = 0; ri < n_entries; ri++) {
          const uint8_t *rela = file_buf + sh_offset + ri * sh_entsize;
          uint32_t r_offset = *(const uint32_t *)(rela + 0);
          uint32_t r_info   = *(const uint32_t *)(rela + 4);
          uint8_t  r_type   = (uint8_t)(r_info & 0xFF);

          if (r_type != 1 && r_type != 6) continue;

          /* Determine which memory region the relocation target is in.
           * For ENABLE_XTENSA_PSRAM_EXEC, relocations in staged_text
           * must be applied there, not in IRAM fallback. */
          volatile uint32_t *target;
#if defined(ENABLE_XTENSA_PSRAM_EXEC)
          if ((p->image.flags & PROC_IMAGE_FLAG_TEXT_STAGED_EXT) &&
              p->image.staged_text.base &&
              r_offset < text_end_va) {
            /* Relocation is in the staged PSRAM text copy */
            target = (volatile uint32_t *)((uintptr_t)p->image.staged_text.base
                                           + r_offset);
          } else
#endif
              if (r_offset < text_end_va)
            target = (volatile uint32_t *)(sram_page + r_offset);
          else if (dram_page && r_offset >= data_va)
            target = (volatile uint32_t *)(dram_page + (r_offset - data_va));
          else
            continue;

          /* Read link-time value and apply active execution base */
          uint32_t val = *target;
          if (val < data_va)
            *target = val + active_text_base;           /* text reference */
          else
            *target = dram_base + (val - data_va); /* data reference → DRAM */
        }
      }
    }

    /* Relocate GOT entries (in DRAM) with active execution base */
    if (dram_page && ehdr->e_shoff && ehdr->e_shnum &&
        ehdr->e_shentsize >= 40) {
      for (uint16_t si = 0; si < ehdr->e_shnum; si++) {
        const uint8_t *sh =
            file_buf + ehdr->e_shoff + si * ehdr->e_shentsize;
        uint32_t sh_type    = *(const uint32_t *)(sh + 4);
        uint32_t sh_flags   = *(const uint32_t *)(sh + 8);
        uint32_t sh_addr    = *(const uint32_t *)(sh + 12);
        uint32_t sh_size    = *(const uint32_t *)(sh + 20);
        uint32_t sh_entsize = *(const uint32_t *)(sh + 36);

        if (sh_type != 1 /* SHT_PROGBITS */) continue;
        if (!(sh_flags & 3 /* SHF_WRITE|SHF_ALLOC */)) continue;
        if (sh_entsize != 4) continue;
        if (sh_addr >= data_va && sh_size > 0) {
          uint32_t n_got = sh_size / 4;
          uint32_t *got = (uint32_t *)(dram_page + (sh_addr - data_va));
          for (uint32_t gi = 0; gi < n_got; gi++) {
            uint32_t val = got[gi];
            if (val == 0 || val == 0xFFFFFFFFu) continue;
            if (val < data_va)
              got[gi] = val + active_text_base;
            else if (val < image_end)
              got[gi] = dram_base + (val - data_va);
          }
        }
      }
    }

    /* Track page-backed DRAM for cleanup and later heap growth. */
    /* Xtensa IRAM text is owned through image.text instead. */
    if (dram_page &&
        proc_track_page_range(p, 0, dram_page, data_pages * PAGE_SIZE) < 0) {
      mem_region_free(&stack_region);
      p->stack_page = NULL;
      p->image.stack = (proc_image_segment_t){0};
      mem_region_free(&staged_text_region);
      p->image.staged_text = (proc_image_segment_t){0};
      mem_region_free(&staged_rodata_region);
      p->image.staged_rodata = (proc_image_segment_t){0};
      mem_region_free(&data_region);
      mem_region_free(&text_region);
      return -(int)ENOMEM;
    }

    /* Set brk base/current after DRAM data for heap growth */
    if (dram_page) {
      p->image.data = proc_image_segment_make_vaddr(
          data_region.base, data_region.size, data_va, PPAP_MEM_RAM_DATA,
          data_region.flags);
      uint32_t data_end = dram_base + data_memsz;
      data_end = (data_end + 15u) & ~15u;
      p->brk_base = data_end;
      p->brk_current = data_end;
    } else {
      uint32_t text_end = iram_base + text_end_va;
      text_end = (text_end + 15u) & ~15u;
      p->brk_base = text_end;
      p->brk_current = text_end;
    }
  }
#elif defined(__riscv)
  /* RISC-V PIC uses auipc (PC-relative) for ALL address references,
   * including writable globals.  This only works when text and data are
   * at their link-time relative offsets.  With XIP (text in flash, data
   * in SRAM), they'd be at completely different addresses.
   *
   * Fix: allocate contiguous SRAM for the entire image (text + data),
   * copy both segments there, and execute from SRAM.  This trades RAM
   * for correctness — no GOT patching needed. */
  {
    uint32_t image_end = text_seg->p_vaddr + text_seg->p_memsz;
    if (data_seg && data_seg->p_vaddr + data_seg->p_memsz > image_end)
      image_end = data_seg->p_vaddr + data_seg->p_memsz;
    uint32_t total_image_pages = (image_end + PAGE_SIZE - 1) / PAGE_SIZE;

    sram_page = page_alloc_contiguous(total_image_pages);
    if (!sram_page) return -(int)ENOMEM;

    /* Allocate kernel stack (separate from user stack via mscratch). */
    stack = page_alloc();
    if (!stack) {
      for (uint32_t i = 0; i < total_image_pages; i++)
        page_free(sram_page + i * PAGE_SIZE);
      return -(int)ENOMEM;
    }
    p->stack_page = stack;
    p->image.stack = proc_image_segment_make(
        stack, PAGE_SIZE, PPAP_MEM_RAM_STACK, PROC_IMAGE_SEG_WRITABLE);

    /* Allocate user stack page, tracked in user_pages[] so vfork
     * shares it naturally (same address in parent and child). */
    void *ustack_rv = page_alloc();
    if (!ustack_rv) {
      page_free(stack);
      for (uint32_t i = 0; i < total_image_pages; i++)
        page_free(sram_page + i * PAGE_SIZE);
      return -(int)ENOMEM;
    }
    p->image.stack = proc_image_segment_make(
        ustack_rv, PAGE_SIZE, PPAP_MEM_RAM_STACK,
        PROC_IMAGE_SEG_WRITABLE);

    /* Zero entire region first (covers BSS and alignment gaps) */
    memset(sram_page, 0, total_image_pages * PAGE_SIZE);
    memset(ustack_rv, 0, PAGE_SIZE);

    /* Copy text segment */
    if (text_seg->p_filesz > 0)
      memcpy(sram_page + text_seg->p_vaddr,
             file_buf + text_seg->p_offset, text_seg->p_filesz);

    /* Copy data segment (initialized portion) */
    if (data_seg && data_seg->p_filesz > 0)
      memcpy(sram_page + data_seg->p_vaddr,
             file_buf + data_seg->p_offset, data_seg->p_filesz);

    entry = (uint32_t)(uintptr_t)(sram_page + e_entry);
    p->image.text = proc_image_segment_make(
        sram_page + text_seg->p_vaddr, text_seg->p_memsz, PPAP_MEM_RAM_TEXT,
        PROC_IMAGE_SEG_EXECUTABLE);
    if (data_seg)
      p->image.data = proc_image_segment_make(
          sram_page + data_seg->p_vaddr, data_seg->p_memsz, PPAP_MEM_RAM_DATA,
          PROC_IMAGE_SEG_WRITABLE);
    p->image.entry = entry;

    if (proc_track_page_range(p, 0, sram_page,
                              total_image_pages * PAGE_SIZE) < 0 ||
        proc_track_page(p, total_image_pages, ustack_rv) < 0) {
      page_free(ustack_rv);
      page_free(stack);
      for (uint32_t i = 0; i < total_image_pages; i++)
        page_free(sram_page + i * PAGE_SIZE);
      p->stack_page = NULL;
      p->image.stack = (proc_image_segment_t){0};
      return -(int)ENOMEM;
    }

    /* Apply R_RISCV_32 relocations from --emit-relocs sections.
     * These fix up absolute addresses in data (function pointer tables
     * like applet_main[]) by adding the SRAM load base.
     *
     * Scan all SHT_RELA sections in the ELF.  For each R_RISCV_32 entry
     * whose target falls within the loaded image, add the load base. */
    uint32_t load_base = (uint32_t)(uintptr_t)sram_page;
    uint32_t riscv32_reloc_count = 0;

    if (ehdr->e_shoff && ehdr->e_shnum && ehdr->e_shentsize >= 40) {
      for (uint16_t si = 0; si < ehdr->e_shnum; si++) {
        const uint8_t *sh =
            file_buf + ehdr->e_shoff + si * ehdr->e_shentsize;
        uint32_t sh_type = *(const uint32_t *)(sh + 4);
        if (sh_type != 4 /* SHT_RELA */) continue;

        uint32_t sh_offset = *(const uint32_t *)(sh + 16);
        uint32_t sh_size   = *(const uint32_t *)(sh + 20);
        uint32_t sh_entsize = *(const uint32_t *)(sh + 36);
        if (sh_entsize < 12) continue; /* RELA entry = 12 bytes */

        uint32_t n_entries = sh_size / sh_entsize;
        for (uint32_t ri = 0; ri < n_entries; ri++) {
          const uint8_t *rela = file_buf + sh_offset + ri * sh_entsize;
          uint32_t r_offset = *(const uint32_t *)(rela + 0);
          uint32_t r_info   = *(const uint32_t *)(rela + 4);
          uint8_t  r_type   = (uint8_t)(r_info & 0xFF);

          /* PC-relative, linker-internal, and debug relocs: no patching
           * needed since the entire binary is loaded contiguously. */
          if (r_type == 0  /* R_RISCV_NONE */ ||
              r_type == 16 /* R_RISCV_BRANCH */ ||
              r_type == 17 /* R_RISCV_JAL */ ||
              r_type == 20 /* R_RISCV_GOT_HI20 (PC-relative to GOT) */ ||
              r_type == 23 /* R_RISCV_PCREL_HI20 */ ||
              r_type == 24 /* R_RISCV_PCREL_LO12_I */ ||
              r_type == 25 /* R_RISCV_PCREL_LO12_S */ ||
              r_type == 35 /* R_RISCV_ADD32 */ ||
              r_type == 39 /* R_RISCV_SUB32 */ ||
              r_type == 44 /* R_RISCV_RVC_BRANCH */ ||
              r_type == 45 /* R_RISCV_RVC_JUMP */ ||
              r_type == 51 /* R_RISCV_RELAX */ ||
              (r_type >= 53 && r_type <= 72) /* R_RISCV_SET/SUB debug */)
            continue;

          /* R_RISCV_RELATIVE = 3: PIE base relocation (from -pie).
           * target = addend + load_base.  Replaces the manual GOT scan
           * and R_RISCV_32 fixups when the binary is linked with -pie. */
          if (r_type == 3 && r_offset < image_end) {
            int32_t r_addend = *(const int32_t *)(rela + 8);
            uint32_t *target = (uint32_t *)(sram_page + r_offset);
            *target = (uint32_t)r_addend + load_base;
            riscv32_reloc_count++;
            continue;
          }

          /* R_RISCV_32 = 1: absolute 32-bit data address */
          if (r_type == 1 && r_offset < image_end) {
            uint32_t *target = (uint32_t *)(sram_page + r_offset);
            *target += load_base;
            riscv32_reloc_count++;
            continue;
          }

          /* R_RISCV_HI20 = 26: patch LUI instruction (upper 20 bits)
           * R_RISCV_LO12_I = 27: patch ADDI instruction (lower 12 bits)
           * These form absolute address pairs: lui rd,hi20 / addi rd,rd,lo12
           * The instruction embeds the link-time address; add load_base.
           *
           * These relocations modify text (instructions).  This only works
           * because RISC-V loads text to SRAM (no XIP).  If the target
           * is in read-only flash (romfs XIP), we cannot patch and must
           * fail the exec. */
          /* R_RISCV_HI20 (26) and R_RISCV_LO12_I (27): absolute address
           * split across LUI (upper 20) and ADDI (lower 12) instructions.
           * With --emit-relocs the linker already resolved the address into
           * the instructions.  Extract the link-time value, add load_base,
           * and re-encode.
           *
           * These modify text (instructions) — requires writable SRAM.
           * Fail if text is in read-only flash (romfs XIP). */
          if ((r_type == 26 || r_type == 27) && r_offset < image_end) {
            if (!sram_page) {
              klogf("ELF: text reloc type %u needs writable text\n", r_type);
              page_free(stack);
              page_free(ustack_rv);
              p->stack_page = NULL;
              return -(int)ENOEXEC;
            }
            /* Instructions may be at 2-byte aligned addresses (after
             * compressed insns).  Use memcpy to avoid misaligned traps
             * on Hazard3. */
            uint8_t *ip = sram_page + r_offset;
            uint32_t raw;
            memcpy(&raw, ip, 4);

            if (r_type == 26) {
              /* LUI: imm[31:12] in bits [31:12].  Add load_base upper
               * bits, with +0x1000 carry if load_base bit 11 is set
               * (ADDI sign-extension compensation). */
              uint32_t new_hi = (raw & 0xFFFFF000u) +
                                (load_base & 0xFFFFF000u);
              if (load_base & 0x800u)
                new_hi += 0x1000u;
              raw = (raw & 0xFFF) | (new_hi & 0xFFFFF000u);
            } else {
              /* ADDI I-type: imm[11:0] in bits [31:20] (sign-extended).
               * Add load_base's low 12 bits. */
              int32_t old_imm = (int32_t)raw >> 20;
              int32_t new_imm = old_imm + (int32_t)(load_base & 0xFFFu);
              raw = (raw & 0xFFFFF) | (((uint32_t)new_imm & 0xFFFu) << 20);
            }
            memcpy(ip, &raw, 4);
            riscv32_reloc_count++;
            continue;
          }

          /* Unhandled relocation type — fail the exec */
          klogf("ELF: unhandled RISCV reloc type %u at offset %x\n",
                r_type, r_offset);
          page_free(stack);
          page_free(ustack_rv);
          for (uint32_t i = 0; i < total_image_pages; i++)
            page_free(sram_page + i * PAGE_SIZE);
          p->stack_page = NULL;
          return -(int)ENOEXEC;
        }
      }
    }
    /* Also relocate GOT entries.  --emit-relocs doesn't produce R_RISCV_32
     * for GOT slots (the linker resolves them directly).  Scan section
     * headers for .got (SHT_PROGBITS, SHF_WRITE, SHF_ALLOC) and add
     * load_base to every non-zero, non-0xFFFFFFFF entry. */
    for (uint16_t si = 0; si < ehdr->e_shnum; si++) {
      const uint8_t *sh =
          file_buf + ehdr->e_shoff + si * ehdr->e_shentsize;
      uint32_t sh_type  = *(const uint32_t *)(sh + 4);
      uint32_t sh_flags = *(const uint32_t *)(sh + 8);
      uint32_t sh_addr  = *(const uint32_t *)(sh + 12);
      uint32_t sh_size  = *(const uint32_t *)(sh + 20);
      uint32_t sh_entsize = *(const uint32_t *)(sh + 36);

      /* .got: PROGBITS, WRITE+ALLOC, entsize=4 */
      if (sh_type != 1 /* SHT_PROGBITS */) continue;
      if (!(sh_flags & 3 /* SHF_WRITE|SHF_ALLOC */)) continue;
      if (sh_entsize != 4) continue;
      if (sh_addr < image_end && sh_size > 0) {
        uint32_t n_got = sh_size / 4;
        uint32_t *got = (uint32_t *)(sram_page + sh_addr);
        for (uint32_t gi = 0; gi < n_got; gi++) {
          if (got[gi] != 0 && got[gi] != 0xFFFFFFFFu &&
              got[gi] < image_end) {
            got[gi] += load_base;
            riscv32_reloc_count++;
          }
        }
      }
    }

    /* Set brk base/current for heap allocation (busybox needs this).
     * brk starts after the end of the loaded image in SRAM. */
    if (data_seg) {
      uint32_t data_end =
          (uint32_t)(uintptr_t)sram_page + data_seg->p_vaddr +
          data_seg->p_memsz;
      data_end = (data_end + 15u) & ~15u;
      p->brk_base = data_end;
      p->brk_current = data_end;
    } else {
      uint32_t text_end =
          (uint32_t)(uintptr_t)sram_page + text_seg->p_vaddr +
          text_seg->p_memsz;
      text_end = (text_end + 15u) & ~15u;
      p->brk_base = text_end;
      p->brk_current = text_end;
    }
  }
#else
  /* ARM/m68k: allocate stack before data pages (no brk adjacency issue
   * because ARM uses GOT-based PIC, not PC-relative data access). */
  stack = page_alloc();
  if (!stack) return -(int)ENOMEM;
  p->stack_page = stack;
  p->image.stack = proc_image_segment_make(
      stack, PAGE_SIZE, PPAP_MEM_RAM_STACK, PROC_IMAGE_SEG_WRITABLE);

  if (cpu_ops->arch_id == CPU_ARCH_M68K) {
    user_stack = page_alloc();
    if (!user_stack) {
      page_free(stack);
      p->stack_page = NULL;
      return -(int)ENOMEM;
    }
    p->image.stack = proc_image_segment_make(
        user_stack, PAGE_SIZE, PPAP_MEM_RAM_STACK,
        PROC_IMAGE_SEG_WRITABLE);
  }

  if (cpu_ops->arch_id == CPU_ARCH_M68K) {
    entry = xip_text_base + e_entry - text_seg->p_vaddr;
  } else {
    entry = xip_text_base + (e_entry & ~1u) - text_seg->p_vaddr;
    entry |= (e_entry & 1u);
  }
  p->image.text = proc_image_segment_make(
      (void *)(uintptr_t)xip_text_base, text_seg->p_memsz, PPAP_MEM_ROM_TEXT,
      PROC_IMAGE_SEG_EXECUTABLE | PROC_IMAGE_SEG_XIP);
  p->image.entry = entry;

  if (data_seg && data_seg->p_memsz > 0) {
    uint32_t data_pages = (data_seg->p_memsz + PAGE_SIZE - 1) / PAGE_SIZE;
    if (data_pages > USER_PAGES_MAX) {
      page_free(stack);
      if (user_stack) page_free(user_stack);
      p->stack_page = NULL;
      return -(int)ENOMEM;
    }

    sram_page = page_alloc_contiguous(data_pages);
    if (!sram_page) {
      page_free(stack);
      if (user_stack) page_free(user_stack);
      p->stack_page = NULL;
      return -(int)ENOMEM;
    }

    if (data_seg->p_filesz > 0)
      memcpy(sram_page, file_buf + data_seg->p_offset, data_seg->p_filesz);

    if (data_seg->p_memsz > data_seg->p_filesz)
      memset(sram_page + data_seg->p_filesz, 0,
             data_seg->p_memsz - data_seg->p_filesz);

    if (elf_find_got(ehdr, file_buf, &got_info, file_size) == 0) {
      uint32_t got_offset_in_data = got_info.addr - data_seg->p_vaddr;
      got_sram_addr = (uint32_t)(uintptr_t)sram_page + got_offset_in_data;
      uint32_t n_entries = got_info.size / 4;

      for (uint32_t i = 0; i < n_entries; i++) {
        uint32_t word_addr = got_sram_addr + (i * 4);
        uint32_t val = cpu_ops->read32(cpu_state, word_addr);
        if (val == 0) continue;
        if (val < data_seg->p_vaddr)
          cpu_ops->write32(cpu_state, word_addr, val + xip_text_base);
        else
          cpu_ops->write32(
              cpu_state, word_addr,
              val - data_seg->p_vaddr + (uint32_t)(uintptr_t)sram_page);
      }
    }

    if (apply_relocations(ehdr, file_buf, file_size, text_seg, data_seg,
                          sram_page, xip_text_base, got_sram_addr, &got_info,
                          cpu_ops, cpu_state) < 0) {
      page_free(stack);
      if (user_stack) page_free(user_stack);
      for (uint32_t i = 0; i < data_pages; i++)
        page_free(sram_page + i * PAGE_SIZE);
      p->stack_page = NULL;
      return -(int)ENOEXEC;
    }

    if (proc_track_page_range(p, 0, sram_page, data_pages * PAGE_SIZE) < 0) {
      page_free(stack);
      if (user_stack) page_free(user_stack);
      for (uint32_t i = 0; i < data_pages; i++)
        page_free(sram_page + i * PAGE_SIZE);
      p->stack_page = NULL;
      p->image.stack = (proc_image_segment_t){0};
      return -(int)ENOMEM;
    }
    p->image.data = proc_image_segment_make(
        sram_page, data_seg->p_memsz, PPAP_MEM_RAM_DATA,
        PROC_IMAGE_SEG_WRITABLE);

    if (cpu_ops->arch_id == CPU_ARCH_M68K) {
#if defined(__m68k__)
      p->user_stack_page = user_stack;
#endif
    }

    uint32_t data_end = (uint32_t)(uintptr_t)sram_page + data_seg->p_memsz;
    data_end = (data_end + 15u) & ~15u;
    p->brk_base = data_end;
    p->brk_current = data_end;
  }
#endif /* !__riscv && !__xtensa__ */

  uint32_t argv_sp = 0;
  uint32_t stack_top;
#if defined(__riscv)
  /* RISC-V: argv on user stack (separate from kernel stack) */
  {
    void *us = proc_last_page_backed_base(p);
    stack_top = (uint32_t)(uintptr_t)us + PAGE_SIZE;
  }
#else
  stack_top = (cpu_ops->arch_id == CPU_ARCH_M68K)
                  ? (uint32_t)(uintptr_t)user_stack + PAGE_SIZE
                  : (uint32_t)(uintptr_t)stack + PAGE_SIZE;
#endif
  uint32_t sp = stack_top;

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

  if (cpu_ops->arch_id == CPU_ARCH_M68K) {
    sp &= ~3u;
  } else {
    sp &= ~7u;
    if ((argc + 7) & 1) sp -= 4;
  }

  sp -= 4;
  *(uint32_t *)(uintptr_t)sp = 0;
  sp -= 4;
  *(uint32_t *)(uintptr_t)sp = 0;
  sp -= 4;
  *(uint32_t *)(uintptr_t)sp = PAGE_SIZE;
  sp -= 4;
  *(uint32_t *)(uintptr_t)sp = 6;
  sp -= 4;
  *(uint32_t *)(uintptr_t)sp = 0;
  sp -= 4;
  *(uint32_t *)(uintptr_t)sp = 0;
  for (int i = argc - 1; i >= 0; i--) {
    sp -= 4;
    *(uint32_t *)(uintptr_t)sp = str_addrs[i];
  }
  sp -= 4;
  *(uint32_t *)(uintptr_t)sp = (uint32_t)argc;
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
          ehdr->e_type == ET_DYN && proc_page_backed_base(p)) {
        /* RISC-V ePIC: ET_DYN (PIE) with no GOT — gp-relative data.
         * Set gp = load_base so crt0 can bootstrap __global_pointer$.
         * GCC non-PIE (ET_EXEC, no GOT) and GCC PIE (ET_DYN, has GOT)
         * don't use gp for data access, so this only fires for ePIC. */
        sw[1] = (uint32_t)(uintptr_t)proc_page_backed_base(p);
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
