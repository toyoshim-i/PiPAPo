/*
 * loader.c — Binary format loader registry
 */

#include "kernel/core/exec/loader.h"

#include "kernel/core/exec/elf_loader.h"
#ifdef PPAP_ENABLE_CPM
#include "kernel/core/exec/com_loader.h"
#endif
#ifdef PPAP_ENABLE_HUMAN68K
#include "kernel/core/exec/r_loader.h"
#include "kernel/core/exec/x_loader.h"
#endif
#ifdef PPAP_ENABLE_SOS
#include "kernel/core/exec/sos_loader.h"
#endif
#ifdef PPAP_ENABLE_ECPU_M68K
#include "kernel/core/exec/m68k_emu_loader.h"
#endif
#ifdef PPAP_ENABLE_MSDOS
#include "kernel/core/exec/dos_com_loader.h"
#endif
#if defined(__ia16__)
extern const loader_t elf16_loader;
extern const loader_t flat_loader;
#endif
#include <stddef.h>

const loader_t* loader_registry[] = {
#ifdef PPAP_ENABLE_CPM
    &com_loader,
#endif
#ifdef PPAP_ENABLE_HUMAN68K
    &x_loader,
    &r_loader,
#endif
#ifdef PPAP_ENABLE_SOS
    &sos_loader,
#endif
#ifdef PPAP_ENABLE_ECPU_M68K
    &m68k_emu_loader,
#endif
#if !defined(__ia16__)
    &elf_loader,
#endif
#ifdef PPAP_ENABLE_MSDOS
    &dos_com_loader, /* DOS .COM before flat (extension match) */
#endif
#if defined(__ia16__)
    &elf16_loader, /* ELF before flat (ELF detection is stricter) */
    &flat_loader,
#endif
    NULL};
