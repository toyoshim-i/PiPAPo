/*
 * elf_loader.c — ELF binary format loader
 */

#include "elf_loader.h"

#include <string.h>

#include "arch/arch.h"
#include "elf.h"
#include "kernel/common/errno.h"
#include "kernel/klog.h"
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

  for (int i = 0; i < nseg && i < MAX_LOAD_SEGS; i++) {
    if (segs[i].p_flags & PF_X)
      text_seg = &segs[i];
    else if (segs[i].p_flags & PF_W)
      data_seg = &segs[i];
  }

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
    uint32_t text_end_va = text_seg->p_vaddr + text_seg->p_memsz;
    uint32_t text_size = (text_end_va + 15u) & ~15u;
    uint32_t data_va = data_seg ? data_seg->p_vaddr : text_end_va;
    uint32_t data_memsz = data_seg ? data_seg->p_memsz : 0;
    uint32_t data_size = (data_memsz + 15u) & ~15u;

    extern void *heap_caps_malloc(unsigned int size, uint32_t caps);
    extern void heap_caps_free(void *ptr);

    /* Allocate IRAM for text (executable, word-access only) */
    sram_page = (uint8_t *)heap_caps_malloc(text_size, (1u << 0));
    if (!sram_page) return -(int)ENOMEM;

    /* Allocate DRAM for data (byte-accessible, needed for .rodata strings) */
    uint8_t *dram_page = NULL;
    uint32_t data_pages = 0;
    if (data_size > 0) {
      data_pages = (data_size + PAGE_SIZE - 1) / PAGE_SIZE;
      if (data_pages == 1) {
        dram_page = (uint8_t *)page_alloc();
      } else {
        dram_page = page_alloc_contiguous(data_pages);
      }
      if (!dram_page) {
        heap_caps_free(sram_page);
        return -(int)ENOMEM;
      }
    }

    /* Allocate kernel stack page */
    stack = page_alloc();
    if (!stack) {
      if (dram_page) {
        for (uint32_t i = 0; i < data_pages; i++)
          page_free(dram_page + i * PAGE_SIZE);
      }
      heap_caps_free(sram_page);
      return -(int)ENOMEM;
    }
    p->stack_page = stack;

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
      volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)(sram_page + text_seg->p_vaddr);
      uint32_t words = (text_seg->p_filesz + 3) / 4;
      for (uint32_t w = 0; w < words; w++) {
        uint32_t val = src[w * 4];
        if (w * 4 + 1 < text_seg->p_filesz) val |= (uint32_t)src[w * 4 + 1] << 8;
        if (w * 4 + 2 < text_seg->p_filesz) val |= (uint32_t)src[w * 4 + 2] << 16;
        if (w * 4 + 3 < text_seg->p_filesz) val |= (uint32_t)src[w * 4 + 3] << 24;
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

    /* Entry point is in IRAM */
    entry = iram_base + e_entry;

    /* Apply split relocations from --emit-relocs sections.
     *
     * Link-time layout: text at vaddr [0, text_end_va), data at [data_va, ...).
     * Runtime: text at iram_base, data at dram_base.
     *
     * For each R_XTENSA_32/PLT relocation, the patched word contains a
     * link-time address V.  Convert to runtime:
     *   V < data_va  → iram_base + V  (text/code reference)
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

          /* Determine which memory region the relocation target is in */
          volatile uint32_t *target;
          if (r_offset < text_end_va)
            target = (volatile uint32_t *)(sram_page + r_offset);
          else if (dram_page && r_offset >= data_va)
            target = (volatile uint32_t *)(dram_page + (r_offset - data_va));
          else
            continue;

          /* Read link-time value and apply split base */
          uint32_t val = *target;
          if (val < data_va)
            *target = val + iram_base;           /* text reference → IRAM */
          else
            *target = dram_base + (val - data_va); /* data reference → DRAM */
        }
      }
    }

    /* Relocate GOT entries (in DRAM) with split bases */
    if (dram_page && ehdr->e_shoff && ehdr->e_shnum && ehdr->e_shentsize >= 40) {
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
              got[gi] = val + iram_base;
            else if (val < image_end)
              got[gi] = dram_base + (val - data_va);
          }
        }
      }
    }

    /* Track IRAM allocation for cleanup (user_page_free detects IRAM range) */
    p->user_pages[0] = sram_page;
    /* Track each DRAM page individually for cleanup via page_free */
    if (dram_page) {
      for (uint32_t i = 0; i < data_pages && (i + 1) < USER_PAGES_MAX; i++)
        p->user_pages[1 + i] = dram_page + i * PAGE_SIZE;
    }

    /* Set brk base/current after DRAM data for heap growth */
    if (dram_page) {
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

    /* Allocate user stack page, tracked in user_pages[] so vfork
     * shares it naturally (same address in parent and child). */
    void *ustack_rv = page_alloc();
    if (!ustack_rv) {
      page_free(stack);
      for (uint32_t i = 0; i < total_image_pages; i++)
        page_free(sram_page + i * PAGE_SIZE);
      return -(int)ENOMEM;
    }

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

    for (uint32_t i = 0; i < total_image_pages; i++)
      p->user_pages[i] = sram_page + i * PAGE_SIZE;
    /* User stack page follows the image pages */
    if (total_image_pages < USER_PAGES_MAX)
      p->user_pages[total_image_pages] = ustack_rv;

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
          (uint32_t)(uintptr_t)sram_page + data_seg->p_vaddr + data_seg->p_memsz;
      data_end = (data_end + 15u) & ~15u;
      p->brk_base = data_end;
      p->brk_current = data_end;
    } else {
      uint32_t text_end =
          (uint32_t)(uintptr_t)sram_page + text_seg->p_vaddr + text_seg->p_memsz;
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

  if (cpu_ops->arch_id == CPU_ARCH_M68K) {
    user_stack = page_alloc();
    if (!user_stack) {
      page_free(stack);
      p->stack_page = NULL;
      return -(int)ENOMEM;
    }
  }

  if (cpu_ops->arch_id == CPU_ARCH_M68K) {
    entry = xip_text_base + e_entry - text_seg->p_vaddr;
  } else {
    entry = xip_text_base + (e_entry & ~1u) - text_seg->p_vaddr;
    entry |= (e_entry & 1u);
  }

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

    for (uint32_t i = 0; i < data_pages; i++)
      p->user_pages[i] = sram_page + i * PAGE_SIZE;

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
    void *us = NULL;
    for (int i = USER_PAGES_MAX - 1; i >= 0; i--)
      if (p->user_pages[i]) { us = p->user_pages[i]; break; }
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
    if (got_sram_addr && cpu_ops->arch_id != CPU_ARCH_XTENSA) {
      /* ARM/RISC-V: patch dedicated PIC register (r9/gp) in initial frame.
       * Xtensa: GOT accessed via L32R (PC-relative literals), no register. */
      uint32_t *sw = (uint32_t *)(uintptr_t)p->sp;
      sw[5] = got_sram_addr;
    }
    p->got_base = got_sram_addr;
  }

  return 0;
}

const loader_t elf_loader = {
    .name = "elf",
    .detect = elf_detect,
    .load = elf_load,
    .required_arch_id = 0,
    .xip = 1,
};
