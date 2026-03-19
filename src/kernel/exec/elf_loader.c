/*
 * elf_loader.c — ELF binary format loader
 */

#include "elf_loader.h"
#include "elf.h"
#include "kernel/errno.h"
#include "kernel/proc/proc.h"
#include "kernel/mm/page.h"
#include "arch/arch.h"
#include <string.h>

static void apply_relocations(const elf32_ehdr_t *ehdr,
                              const uint8_t *file_base,
                              uint32_t file_size,
                              const elf32_phdr_t *text_seg,
                              const elf32_phdr_t *data_seg,
                              uint8_t *sram_page,
                              uint32_t text_base,
                              uint32_t got_sram_addr,
                              const elf_got_info_t *got_info,
                              const cpu_ops_t *cpu_ops,
                              void *cpu_state)
{
    elf_rel_info_t rel_info;
    if (elf_find_rel(ehdr, file_base, &rel_info, file_size) != 0)
        return;

    const uint8_t *rel_base = file_base + rel_info.offset;
    uint32_t entry_size = rel_info.has_addend
        ? sizeof(elf32_rela_t) : sizeof(elf32_rel_t);
    uint32_t n_rel = rel_info.size / entry_size;

    elf_dynsym_info_t dynsym_info = {0, 0};
    const elf32_sym_t *dynsym = NULL;
    if (elf_find_dynsym(ehdr, file_base, &dynsym_info, file_size) == 0)
        dynsym = (const elf32_sym_t *)(file_base + dynsym_info.offset);

    for (uint32_t i = 0; i < n_rel; i++) {
        uint32_t r_offset, r_info;
        int32_t  r_addend = 0;

        if (rel_info.has_addend) {
            const elf32_rela_t *rela = (const elf32_rela_t *)(rel_base + i * entry_size);
            r_offset = rela->r_offset;
            r_info   = rela->r_info;
            r_addend = rela->r_addend;
        } else {
            const elf32_rel_t *rel = (const elf32_rel_t *)(rel_base + i * entry_size);
            r_offset = rel->r_offset;
            r_info   = rel->r_info;
        }

        uint32_t rtype = ELF32_R_TYPE(r_info);

        if ((cpu_ops->arch_id == CPU_ARCH_M68K && rtype == R_68K_JMP_SLOT) ||
            (cpu_ops->arch_id == CPU_ARCH_ARM && rtype == R_ARM_JMP_SLOT)) {
            if (!dynsym) continue;
            uint32_t sym_idx = ELF32_R_SYM(r_info);
            if (sym_idx >= dynsym_info.count) continue;
            uint32_t off = r_offset;
            if (off < data_seg->p_vaddr || off >= data_seg->p_vaddr + data_seg->p_memsz) continue;
            uint32_t off_in_sram = off - data_seg->p_vaddr;
            uint32_t word_addr = (uint32_t)(uintptr_t)sram_page + off_in_sram;
            
            uint32_t sym_val = dynsym[sym_idx].st_value + (uint32_t)r_addend;
            if (sym_val < data_seg->p_vaddr) {
                cpu_ops->write32(cpu_state, word_addr, sym_val + text_base);
            } else {
                cpu_ops->write32(cpu_state, word_addr, sym_val - data_seg->p_vaddr + (uint32_t)(uintptr_t)sram_page);
            }
            continue;
        }

        if ((cpu_ops->arch_id == CPU_ARCH_M68K && rtype != R_68K_RELATIVE) &&
            (cpu_ops->arch_id == CPU_ARCH_ARM && rtype != R_ARM_RELATIVE) &&
            (cpu_ops->arch_id == CPU_ARCH_ARMV6 && rtype != R_ARM_RELATIVE))
            continue;

        uint32_t off = r_offset;

        if (off < data_seg->p_vaddr || off >= data_seg->p_vaddr + data_seg->p_memsz) continue;
        if (got_sram_addr != 0 && got_info && off >= got_info->addr && off < got_info->addr + got_info->size) continue;

        uint32_t off_in_sram = off - data_seg->p_vaddr;
        uint32_t word_addr = (uint32_t)(uintptr_t)sram_page + off_in_sram;

        uint32_t word_val = cpu_ops->read32(cpu_state, word_addr);
        uint32_t val = rel_info.has_addend ? (uint32_t)r_addend : word_val;

        if (val == 0) continue;

        if (val < data_seg->p_vaddr) {
            cpu_ops->write32(cpu_state, word_addr, val + text_base);
        } else {
            cpu_ops->write32(cpu_state, word_addr, val - data_seg->p_vaddr + (uint32_t)(uintptr_t)sram_page);
        }
    }
}

static int elf_detect(const uint8_t* file_buf, uint32_t file_size, const char* path) {
    (void)path;
    if (file_size < sizeof(elf32_ehdr_t)) return 0;
    const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)file_buf;
    return elf_validate(ehdr) == 0;
}

#define MAX_LOAD_SEGS 4

static int elf_load(pcb_t* p, const uint8_t* file_buf, uint32_t file_size,
                    const cpu_ops_t* cpu_ops, void* cpu_state,
                    const char* const* argv) {
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
    uint32_t text_base;
    uint8_t *sram_page = NULL;
    uint32_t got_sram_addr = 0;
    elf_got_info_t got_info = {0, 0, 0};

    void *stack = page_alloc();
    if (!stack) return -(int)ENOMEM;
    p->stack_page = stack;

    void *user_stack = NULL;
    if (cpu_ops->arch_id == CPU_ARCH_M68K) {
        user_stack = page_alloc();
        if (!user_stack) {
            page_free(stack);
            p->stack_page = NULL;
            return -(int)ENOMEM;
        }
    }

    uint32_t xip_text_base = (uint32_t)(uintptr_t)file_buf + text_seg->p_offset;
    text_base = xip_text_base;

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
            memset(sram_page + data_seg->p_filesz, 0, data_seg->p_memsz - data_seg->p_filesz);

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
                    cpu_ops->write32(cpu_state, word_addr, val - data_seg->p_vaddr + (uint32_t)(uintptr_t)sram_page);
            }
        }

        apply_relocations(ehdr, file_buf, file_size, text_seg, data_seg,
                          sram_page, xip_text_base, got_sram_addr, &got_info, cpu_ops, cpu_state);

        for (uint32_t i = 0; i < data_pages; i++)
            p->user_pages[i] = sram_page + i * PAGE_SIZE;

        if (cpu_ops->arch_id == CPU_ARCH_M68K) {
#if defined(__m68k__)
            p->user_stack_page = user_stack;
#endif
        }

        uint32_t data_end = (uint32_t)(uintptr_t)sram_page + data_seg->p_memsz;
        data_end = (data_end + 15u) & ~15u;
        p->brk_base    = data_end;
        p->brk_current = data_end;
    }

    uint32_t argv_sp = 0;
    uint32_t stack_top = (cpu_ops->arch_id == CPU_ARCH_M68K)
        ? (uint32_t)(uintptr_t)user_stack + PAGE_SIZE
        : (uint32_t)(uintptr_t)stack + PAGE_SIZE;
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

    sp -= 4; *(uint32_t *)(uintptr_t)sp = 0;
    sp -= 4; *(uint32_t *)(uintptr_t)sp = 0;
    sp -= 4; *(uint32_t *)(uintptr_t)sp = PAGE_SIZE;
    sp -= 4; *(uint32_t *)(uintptr_t)sp = 6;
    sp -= 4; *(uint32_t *)(uintptr_t)sp = 0;
    sp -= 4; *(uint32_t *)(uintptr_t)sp = 0;
    for (int i = argc - 1; i >= 0; i--) {
        sp -= 4; *(uint32_t *)(uintptr_t)sp = str_addrs[i];
    }
    sp -= 4; *(uint32_t *)(uintptr_t)sp = (uint32_t)argc;
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
        if (p == current)
            exec_pending[0] = 1;
        arch_irq_restore(irq_save);
    } else {
        proc_setup_stack(p, (void (*)(void))(uintptr_t)entry, argv_sp);
        if (got_sram_addr) {
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
};
