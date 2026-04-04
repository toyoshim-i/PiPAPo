/*
 * common/core/page_types.h — Page ID type and constants
 *
 * Extracted from core/mm/page.h for cross-module use.
 * Data only — no function declarations.
 */

#ifndef PPAP_KERNEL_COMMON_CORE_PAGE_TYPES_H
#define PPAP_KERNEL_COMMON_CORE_PAGE_TYPES_H

#include <stdint.h>

#include "kernel/common/config.h" /* PAGE_SIZE */

#if defined(__ia16__)
typedef uint8_t page_id_t;
#define PAGE_ID_INVALID ((page_id_t)0xFFu)
#elif defined(__m68k__)
typedef uint16_t page_id_t;
#define PAGE_ID_INVALID ((page_id_t)0xFFFFu)
#else
typedef uint32_t page_id_t;
#define PAGE_ID_INVALID ((page_id_t)0xFFFFFFFFu)
#endif

#endif /* PPAP_KERNEL_COMMON_CORE_PAGE_TYPES_H */
