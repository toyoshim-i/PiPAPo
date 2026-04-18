/*
 * loader.c — Binary format loader registry
 */

#include "kernel/core/exec/loader.h"

#include "kernel/core/exec/elf_loader.h"
#ifdef PPAP_ENABLE_CPM
#include "kernel/core/subsys/cpm/cpm_loader.h"
#endif
#ifdef PPAP_ENABLE_HUMAN68K
#include "kernel/core/subsys/human68k/r_loader.h"
#include "kernel/core/subsys/human68k/x_loader.h"
#endif
#ifdef PPAP_ENABLE_SOS
#include "kernel/core/subsys/sos/sos_loader.h"
#endif
#ifdef PPAP_ENABLE_ECPU_M68K
#include "kernel/core/subsys/ppap/m68k_emu_loader.h"
#endif
#ifdef PPAP_ENABLE_MSDOS
#include "kernel/core/subsys/msdos/com_loader.h"
#include "kernel/core/subsys/msdos/exe_loader.h"
#endif
#if defined(__ia16__)
extern const loader_t elf16_loader;
extern const loader_t flat_loader;
#endif
#include <stddef.h>

const loader_t* loader_registry[] = {
#ifdef PPAP_ENABLE_CPM
    &cpm_loader,
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
    &exe_loader, /* DOS .EXE (MZ signature match) */
    &com_loader, /* DOS .COM before flat (extension match) */
#endif
#if defined(__ia16__)
    &elf16_loader, /* ELF before flat (ELF detection is stricter) */
    &flat_loader,
#endif
    NULL};
