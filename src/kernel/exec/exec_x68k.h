/*
 * exec_x68k.h — Human68k X-format binary loader
 *
 * Detects and loads Human68k X-format (.x) executables.  On native m68k,
 * the binary runs directly with F-line exceptions intercepted by the kernel
 * for DOS call translation.
 */

#ifndef PPAP_EXEC_EXEC_X68K_H
#define PPAP_EXEC_EXEC_X68K_H

#include "kernel/proc/proc.h"
#include <stdint.h>

/* X-format magic: "HU" (0x48 0x55) at offset 0 */
#define X68K_MAGIC_0  0x48
#define X68K_MAGIC_1  0x55

/* X-format header size */
#define X68K_HEADER_SIZE  64

/*
 * X-format executable header (64 bytes, big-endian).
 *
 * All multi-byte fields are big-endian (m68k native).  On native m68k
 * builds they can be read directly; cross-arch builds must byte-swap.
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic[2];          /* 0x00: "HU" (0x48 0x55)               */
    uint8_t  load_mode;         /* 0x02: reserved (load mode)            */
    uint8_t  load_addr_flag;    /* 0x03: reserved (load address flag)    */
    uint32_t base_addr;         /* 0x04: base address (usually 0)        */
    uint32_t entry_offset;      /* 0x08: entry point offset from base    */
    uint32_t text_size;         /* 0x0C: text segment size               */
    uint32_t data_size;         /* 0x10: data segment size               */
    uint32_t bss_size;          /* 0x14: BSS segment size                */
    uint32_t reloc_size;        /* 0x18: relocation table size           */
    uint32_t sym_size;          /* 0x1C: symbol table size               */
    uint32_t scd_line_size;     /* 0x20: SCD line number table (debug)   */
    uint32_t scd_sym_size;      /* 0x24: SCD symbol table (debug)        */
    uint32_t scd_str_size;      /* 0x28: SCD string table (debug)        */
    uint32_t reserved[4];       /* 0x2C–0x3B: reserved (0)               */
    uint32_t bind_list;         /* 0x3C: bind list address (0 if none)   */
} x68k_header_t;

_Static_assert(sizeof(x68k_header_t) == X68K_HEADER_SIZE,
               "x68k_header_t must be exactly 64 bytes");

/*
 * x68k_detect — check if file starts with the "HU" magic.
 *
 * Returns 1 if the file is an X-format Human68k binary, 0 otherwise.
 */
int x68k_detect(const uint8_t *file, uint32_t size);

/*
 * x68k_validate — validate an X-format header.
 *
 * Checks magic, minimum file size (header + text + data + reloc),
 * and that segment sizes are sane.
 *
 * Returns 0 on success, negative errno on failure.
 */
int x68k_validate(const x68k_header_t *hdr, uint32_t file_size);

/*
 * x68k_apply_relocs — process X-format relocation table.
 *
 * Walks the relocation chain and adds `delta` to each fixup location.
 * `image` points to the loaded text+data in memory (writable copy).
 * `reloc_table` points to the relocation data from the file.
 *
 * Returns 0 on success, negative errno if the table is malformed.
 */
int x68k_apply_relocs(uint8_t *image, uint32_t image_size,
                      const uint8_t *reloc_table, uint32_t reloc_size,
                      uint32_t delta);

/*
 * exec_x68k — load and set up an X-format binary for execution.
 *
 * Called from do_execve() when x68k_detect() succeeds.
 * On success returns 0; the PCB is ready and the caller sets RUNNABLE.
 * On failure returns negative errno.
 */
int exec_x68k(pcb_t *p, const uint8_t *file, uint32_t size,
              const char *path, const char *const *argv);

#endif /* PPAP_EXEC_EXEC_X68K_H */
