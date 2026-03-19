/*
 * loader.c — Binary format loader registry
 */

#include "loader.h"
#include "elf_loader.h"
#include <stddef.h>

// This will hold pointers to all available loader implementations.
const loader_t* loader_registry[] = {
    &elf_loader,
    NULL
};
