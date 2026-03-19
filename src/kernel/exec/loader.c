/*
 * loader.c — Binary format loader registry
 */

#include "loader.h"
#include "elf_loader.h"
#ifdef PPAP_ENABLE_CPM
#include "com_loader.h"
#endif
#include <stddef.h>

const loader_t* loader_registry[] = {
#ifdef PPAP_ENABLE_CPM
    &com_loader,
#endif
    &elf_loader,
    NULL
};
