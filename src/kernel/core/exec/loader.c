/*
 * loader.c — Binary format loader registry
 */

#include "loader.h"

#include "elf_loader.h"
#ifdef PPAP_ENABLE_CPM
#include "com_loader.h"
#endif
#ifdef PPAP_ENABLE_HUMAN68K
#include "r_loader.h"
#include "x_loader.h"
#endif
#ifdef PPAP_ENABLE_SOS
#include "sos_loader.h"
#endif
#ifdef PPAP_ENABLE_ECPU_M68K
#include "m68k_emu_loader.h"
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
    &x_loader,        &r_loader,
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
#if defined(__ia16__)
    &elf16_loader,   /* ELF before flat (ELF detection is stricter) */
    &flat_loader,
#endif
    NULL};
